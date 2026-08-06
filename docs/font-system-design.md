# 中文字体显示系统设计方案（可移植参考）

> 本文档总结 xiaozhi-esp32 项目中文字体显示系统的**完整实现方案**，作为**可移植设计参考**，供其他 ESP32/LVGL 项目借鉴。
> 方案核心：**双层字体架构（编译内置 + 运行时 assets 覆盖）+ 零拷贝 flash 映射渲染**。
>
> 适用目标：ESP32 系列 + LVGL（v8/v9），单色或彩色 LCD，中文界面。
> 关联代码：`main/assets.cc`、`main/display/lvgl_display/*`、`scripts/build_default_assets.py`、`managed_components/78__xiaozhi-fonts/`。

---

## 1. 背景与目标

嵌入式中文显示的核心矛盾：

| 问题 | 说明 |
|---|---|
| **体积** | 全量中文字库 1.7MB~15MB（14px~30px），远超固件可承受范围 |
| **RAM** | 字形位图动辄数百 KB~数 MB，不能常驻 RAM |
| **覆盖率** | 界面静态文案 + 动态内容（LLM 回答）字符集需求不同 |
| **可升级** | 字库更新不应要求重刷固件 |

### 设计目标
1. 基础中文显示**零额外 RAM**（字形从 flash 直读）
2. 小字库内置保证可用，**全量字库可运行时升级**（OTA）
3. 字符集按需分级（静态界面子集 / 全量动态子集）
4. 移植到新项目成本最低（依赖 + 配置 + 分区）

---

## 2. 架构总览（双层字体）

```
┌────────────────────────────────────────────────────────────────┐
│  层 1：编译内置字体（保证可用）                                  │
│  font_puhui_basic_<size>_<bpp>.c  ──→  固件 .rodata（const）    │
│  · 静态界面子集 ~314 字（拉丁 + 界面中文字符串）                 │
│  · 零 RAM：字形直接 const 访问                                  │
└────────────────────────────────────────────────────────────────┘
                              ▲ 默认
                              │
┌────────────────────────────────────────────────────────────────┐
│  层 2：运行时字体（按需升级）                                    │
│  font_puhui_common_<size>_<bpp>.bin  ──→  assets 分区（mmap）   │
│  · 全量子集 ~18000+ 字（LLM tokenizer 语料提取）                 │
│  · 零拷贝：字形指针直指 flash 映射区                             │
│  · OTA 可更新（web 工具生成 → 服务器下发）                       │
└────────────────────────────────────────────────────────────────┘
        ▲ index.json "text_font" 字段选择，启动时覆盖层 1
```

**核心决策**：两层都用 **LVGL 原生 `lv_font_fmt_txt` 格式**——层 2 只是层 1 的二进制序列化，运行时反序列化重建 `lv_font_t`。这使 LVGL 渲染路径完全统一，无需自定义绘制。

---

## 3. 字体格式详解

### 3.1 LVGL fmt_txt 格式（两层共用）

标准 LVGL 生成字体由以下结构组成：

```c
// 生成的 C 源码结构（如 font_puhui_basic_14_1.c）
static const uint8_t glyph_bitmap[];        // 打包位图数据（1bpp/4bpp）
static const lv_font_glyph_dsc_t glyph_dsc[]; // 每字形指标（宽/高/偏移/adv_w）
static const lv_font_cmap_t cmaps[];          // Unicode → glyph_id 映射表（多段）
static const lv_font_kern_class_t kern_classes; // 字距
static const lv_font_fmt_txt_dsc_t font_dsc = {
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .bpp = 1,           // 1=单色（单色屏） 4=抗锯齿
    .cmap_num = 7,
};
const lv_font_t font_puhui_basic_14_1 = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 18,
    .fallback = NULL,   // 注意：本项目无 fallback 链
    .dsc = &font_dsc,
};
```

**cmap 关键设计**——中文字符段用**稀疏映射（SPARSE_TINY）**：

```c
cmaps[] = {
    { range_start=32,  range_length=95,    FORMAT0_TINY },   // ASCII
    { range_start=161, range_length=95,    FORMAT0_TINY },   // Latin-1
    { range_start=1102, range_length=64205, SPARSE_TINY },   // ★ CJK 段
    // SPARSE: 仅存实际使用的字（unicode_list + glyph_id_ofs_list）
};
```

稀疏映射使"全量子集 18000 字"和"基础子集 314 字"共用同一格式，只差字表内容。

### 3.2 cbin 二进制格式（运行时字体）

**cbin = `lv_font_fmt_txt` 结构的紧凑序列化**，指针换相对偏移：

```
cbin 文件布局（概念）：
┌─────────────┐
│ lv_font_t   │  ← 复制到堆（lv_malloc），含 dsc 偏移
│ fmt_txt_dsc │  ← 复制到堆
│ cmaps[]     │  ← 复制到堆（20 字节/条目，字段逐个反序列化）
│ unicode_list│  ← 偏移解析为绝对指针 → 指向 mmap 分区数据
│ glyph_bitmap│  ← 偏移解析为绝对指针 → 指向 mmap 分区数据（零拷贝！）
└─────────────┘
```

反序列化入口（`78__xiaozhi-fonts` 组件提供）：

```c
#include "cbin_font.h"
lv_font_t* font = cbin_font_create(const uint8_t* data);  // data = mmap 分区地址
// 成功后 font 可直接用于 lv_obj_set_style_text_font()
```

**关键洞察**：只堆分配**元数据**（lv_font_t + dsc + cmaps ≈ 几百字节）；**字形位图数据零拷贝**，指针直指 flash 映射区。因此全量 18000 字字库运行时也只占几百字节 RAM。

---

## 4. assets 分区格式（自定义 mmap blob）

> 分区表类型标为 `spiffs`（`partitions/v2/16m.csv:8`），但**内容不是 SPIFFS 文件系统**——是自定义内存映射二进制。

### 4.1 布局

```
偏移      大小   字段              说明
─────────────────────────────────────────────────
0x00      4     file_count        uint32_le 文件数
0x04      4     checksum          uint32_le 数据段求和 & 0xFFFF
0x08      4     data_length       uint32_le 表+数据总长
0x0C      44×N  mmap_assets_table[]  文件清单（44 字节/条）
0x0C+44N  var   数据段            文件内容拼接
```

### 4.2 文件清单条目（`main/assets.cc:19-25`）

```c
struct mmap_assets_table {
    char     asset_name[32];   // 文件名（UTF-8，null 填充）
    uint32_t asset_size;       // 文件大小
    uint32_t asset_offset;     // 数据段内偏移
    uint16_t asset_width;      // 图片宽（字体/bin 为 0）
    uint16_t asset_height;     // 图片高（字体/bin 为 0）
};  // 44 字节
```

### 4.3 数据段与校验

- 每个文件数据前缀 **2 字节魔数 `0x5A5A`**（"ZZ"），访问时校验（`assets.cc:524`）
- **16 位累加校验和**覆盖表+数据段（`assets.cc:39-45`）
- 启动时 `esp_partition_mmap()` 映射整个分区（`assets.cc:67`）→ 全量零拷贝访问

### 4.4 index.json 清单

打包为普通文件放入 blob，运行时解析（`assets.cc:107-383`）：

```json
{
    "version": 1,
    "srmodels": "srmodels.bin",
    "text_font": "font_puhui_common_30_4.bin",   // ★ 字体覆盖键
    "emoji_collection": [{"name": "...", "file": "..."}]
}
```

**注意**：仅 `text_font`（正文字体）可被 assets 覆盖；图标字体（`icon_font`/`large_icon_font`）固定内置。

---

## 5. 字体生成流程

> 字体**生成**在上游组件 `78/xiaozhi-fonts`（IDF 组件管理器依赖，`main/idf_component.yml:24`），本仓库只做**打包**。

### 5.1 上游生成（`generate_fonts.ipynb`）

```
字符集提取（~18000 常用字）
  DeepSeek-R1 + Qwen3 tokenizer 语料
        ↓
TTF 子集化（fontTools.subset）
  8 个 Noto 字体（Sans SC/TC/JP/KR/Thai/Arabic/Emoji）→ 合并单 TTF
        ↓
位图转换（lv_font_conv，78/lv_font_conv fork）
  --format lvgl  → .c 源码（内置字体）
  --format cbin  → .bin 二进制（运行时字体）
  --no-compress --no-prefilter --force-fast-kern-format
```

### 5.2 命名规范

```
font_<family>_<variant>_<像素>_<bpp>
例：
  font_puhui_basic_14_1    → 内置（基础子集）
  font_puhui_common_14_1.bin → assets（全量子集）
  font_awesome_14_1        → 图标字体
```

| 规格 | 字符集 | 大小示例 | 用途 |
|---|---|---|---|
| `basic_14_1` | ~314 字 | 217 KB | 单色小屏内置 |
| `basic_16_4/20_4/30_4` | ~314 字 | 511KB~1.3MB | 彩色屏内置 |
| `puhui_14_1`（无 basic） | ~18000 字 | 1.7 MB | 全量内置（大 flash） |
| `common_*.bin` | ~18000 字 | 依规格 | 全量运行时 |

### 5.3 构建打包（`scripts/build_default_assets.py`）

1. 读 sdkconfig → 唤醒词模型名
2. `get_text_font_path()`（L665）：板级 `BUILTIN_TEXT_FONT`（如 `font_puhui_basic_30_4`）→ 替换 `basic`→`common` + `.bin` → 定位 `xiaozhi-fonts/cbin/` 中文件
3. 复制字体 .bin 到临时目录
4. 生成 `index.json`（含 `text_font` 字段）
5. `pack_assets_simple()`（L347）：mmap 格式打包 + 校验和
6. 输出 `generated_assets.bin` → `esptool_py_flash_to_partition(flash "assets" ...)`

### 5.4 板级字体选择（`main/CMakeLists.txt:66-588`）

```cmake
elseif(CONFIG_BOARD_TYPE_WAVESHARE_S3_RLCD_4_2)
    set(BOARD_TYPE "waveshare-s3-rlcd-4.2")
    set(BUILTIN_TEXT_FONT font_puhui_basic_14_1)   # ← 中文正文
    set(BUILTIN_ICON_FONT font_awesome_14_1)        # ← 图标
endif()
# 编译宏注入（L780）：
# target_compile_definitions(... PRIVATE BUILTIN_TEXT_FONT=...)
```

---

## 6. 运行时加载流程

```
开机
 ├─ lcd_display.cc:25  InitializeLcdThemes()
 │    text_font = LvglBuiltInFont(&BUILTIN_TEXT_FONT)   ← 层1 默认
 │    light/dark theme 注册
 ├─ assets.cc:67       esp_partition_mmap()             ← 映射 assets 分区
 ├─ assets.cc:107      Assets::Apply()
 │    index.json 解析 → text_font 字段
 │    GetAssetData(font.bin) → 校验 ZZ 魔数
 │    LvglCBinFont(ptr) → cbin_font_create() → lv_font_t*
 │    light/dark theme->set_text_font(覆盖)             ← 层2 覆盖
 └─ lcd_display.cc:366 lv_obj_set_style_text_font(screen, text_font, 0)
```

**主题抽象**（`lvgl_font.h`）：

```cpp
class LvglFont { virtual const lv_font_t* font() const = 0; };
class LvglBuiltInFont : public LvglFont { /* 包 const 字体符号 */ };
class LvglCBinFont   : public LvglFont { /* 包 cbin_font_create() */ };
```

主题持有三槽位（`lvgl_theme.h:70-72`）：`text_font_`（中文）/ `icon_font_` / `large_icon_font_`。

---

## 7. 中文渲染链路

```
UTF-8 文本 → LVGL lv_text 解码 → Unicode 码点
  → lv_font_get_glyph_dsc_fmt_txt() 查 cmaps[]
     · ASCII/Latin 走 FORMAT0 连续段（O(1)）
     · 中文走 SPARSE_TINY 稀疏段（二分查找 unicode_list）
  → lv_font_get_bitmap_fmt_txt() 按 glyph_id 取位图（flash 直读）
  → LVGL blitter 绘制到 draw buffer（PSRAM）
```

- LVGL 内部有**字形缓存**（`lv_font_fmt_txt_glyph_cache_t`），常用字只解一次
- 本板 1bpp（单色 ST7305）；彩色板 4bpp 抗锯齿

---

## 8. 内存与性能特性

| 项 | 内置字体 | 运行时 cbin 字体 |
|---|---|---|
| 字形存储 | flash `.rodata` | assets 分区（mmap） |
| 字形 RAM | **0** | **0**（mmap 直读） |
| 元数据 RAM | 0（const） | ~几百字节（lv_malloc） |
| 访问 | const 指针 | 指向 mmap flash 区 |
| 缓存 | LVGL 内部字形缓存 | 同左 |
| 全量字库代价 | 固件 +1.7~15MB | **固件 0 成本**，占 assets 分区 |

**本板（waveshare-s3-rlcd-4.2，16MB flash）**：
- 内置 `font_puhui_basic_14_1`：217KB（单色屏 1bpp 足够，4bpp 抗锯齿浪费）
- assets 分区 6.63MB：可容纳 `font_puhui_common_14_1.bin` 全量
- 显示缓冲在 PSRAM，**字体数据永不进 RAM**

---

## 9. 移植指南（新项目 Checklist）

### 依赖
```yaml
# main/idf_component.yml
dependencies:
  78/xiaozhi-fonts: ~1.5.5    # 字体组件（含 cbin 解析 + 预生成字库）
  espressif/esp_mmap_assets: * # assets 分区 mmap 访问
  lvgl/lvgl: *
```

### 分区表
```csv
assets, data, spiffs, 0x960000, 0x6A0000    # 6.63MB（大小按需）
```
> 类型写 spiffs 仅为 ESP-IDF 识别；内容为自定义 mmap blob。

### 板级配置（main/CMakeLists.txt）
```cmake
set(BUILTIN_TEXT_FONT font_puhui_basic_14_1)   # 按屏选：单色14/彩色20/30
set(BUILTIN_ICON_FONT font_awesome_14_1)
# 若 flash 足够且需全量内置：font_puhui_14_1（18000 字）
```

### 屏幕初始化
```cpp
// lcd_display.cc 参考
LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
theme->set_text_font(text_font);
lv_obj_set_style_text_font(screen, text_font->font(), 0);
```

### assets 打包（可选，若需运行时升级）
- 复用 `scripts/build_default_assets.py` 的 `pack_assets_simple()`
- 或集成 `78/xiaozhi-assets-generator` 网页工具输出
- 设备侧解析复用 `main/assets.cc` 的 mmap 表逻辑

### 运行时覆盖（可选）
```cpp
// assets.cc 参考：index.json text_font → cbin_font_create → set_text_font
```

---

## 10. 定制与扩展

| 方式 | 说明 |
|---|---|
| **自定义字库** | `78/xiaozhi-assets-generator` 网页工具：上传 TTF → 选字符集 → 生成 cbin |
| **三种烧录模式** | `FLASH_DEFAULT_ASSETS`（构建自动）/ `FLASH_CUSTOM_ASSETS`（本地或 URL）/ `FLASH_NONE_ASSETS`（首启 OTA） |
| **OTA 更新** | `Assets::Download(url)` 运行时下载 assets.bin 写入分区（`assets.cc:385-516`） |
| **换中文字体族** | 上游 `generate_fonts.ipynb` 换 TTF 源重新生成（Noto / 思源 / 自定义） |

---

## 11. 已知局限与改进（v3.7.0 已解决部分）

> 以下前三项已在 **v3.7.0** 实施解决（详见 CHANGELOG）：

| 局限 | 影响 | 状态（v3.7.0） | 实现 |
|---|---|---|---|
| **无 fallback 链**（`.fallback = NULL`） | 缺字渲染空白 | ✅ **已解决** | assets 全量 cbin 字体（堆分配可写）`fallback` 指向编译内置 basic（`main/assets.cc` Apply 内）；LVGL 9 递归解析 |
| **basic 子集仅 ~314 字** | LLM 动态中文可能缺字 | ✅ **已解决** | 本板 assets 已含 `font_puhui_common_30_4.bin`（全量 18000+ 字）自动覆盖；fallback 为保险层 |
| **仅 text_font 可覆盖** | 图标字体不可运行时换 | ✅ **已解决** | index.json 支持 `icon_font`/`large_icon_font` 键；修复 `LcdDisplay::SetTheme()` 未刷新 3 个图标 label 的 gap |
| **1bpp 无抗锯齿** | 单色屏大字号边缘生硬 | ⏳ 未解决 | 单色屏固有局限；可选 2bpp 折中（需上游重新生成） |
| **cbin 为上游私有格式** | 格式文档依赖组件 | ✅ 缓解 | 已在本文 3.2 记录结构，组件已开源 |
| **缺字无可见提示** | 调试困难 | ✅ **已解决** | `CONFIG_LV_USE_FONT_PLACEHOLDER=y`——缺字渲染可见占位框 |

**关键实现洞察（可移植）**：
- ESP32 上 `const` 字体在 flash **不可写** → 不能直接改 `.fallback`
- 但 **cbin 字体由 `cbin_font_create()` 堆分配（RAM 可写）** → 加载后直接设 `font->fallback` 即可
- fallback 链两字体**必须同像素尺寸**（`<size>_<bpp>` 匹配），否则行高/advance 不一致导致跳动

**待实施建议（如后续需要）**：
1. `2bpp` 折中抗锯齿（单色屏大字号）
2. assets-generator（独立仓库）同步输出 `icon_font`/`large_icon_font` 键

---

## 12. 结论

该方案的核心价值在三点，均适合移植复用：

1. **格式统一**：内置/运行时共用 LVGL 原生格式，渲染路径零定制
2. **零拷贝**：字形永不进 RAM，全量字库运行时只占几百字节元数据
3. **分级+可升级**：小字库保底、全量按需 OTA，字符集按 tokenizer 语料智能提取

移植成本：**依赖 1 个组件 + 分区 1 行 + 板级 2 行配置**即可获得完整中文显示 + 运行时升级能力。
