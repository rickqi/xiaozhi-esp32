# Phone Shell 图标与桌面背景完全指南

> 适用于 ESP32-S3-Touch-AMOLED-2.06（brookesia Phone Shell）
> 目标分辨率：410×502 AMOLED | 构建分支：feature/brookesia-phone

---

## 1. 系统概述

### 1.1 硬件规格

| 项目 | 值 |
|---|---|
| 屏幕 | 2.06 寸 AMOLED |
| 分辨率 | 410 × 502 |
| 触摸 | FT5x06 电容触摸 |
| Flash | 16MB |
| App 分区 | ~4.63MB（双 OTA 槽） |
| Assets 分区 | ~6.63MB（SPIFFS，字体/表情/音频） |

### 1.2 Phone Shell 布局

```
┌──────────────────────┐
│    Status Bar        │  ← 状态栏（时钟/电量/信号）
├──────────────────────┤
│                      │
│  ┌──┐  ┌──┐  ┌──┐  │
│  │📊│  │🎨│  │⚙️│  │  ← App Launcher 图标网格（3列）
│  └──┘  └──┘  └──┘  │     图标显示尺寸 120×120px
│  XiaoZhi FluidBox ...│     标签字体 16px 白色
│                      │
│  ┌──┐  ┌──┐  ┌──┐  │
│  │  │  │  │  │  │  │     图标源数据 112×112px
│  └──┘  └──┘  └──┘  │     （按下缩至 110×110px）
│                      │
├──────────────────────┤
│  ──  Navigation Bar  │  ← 导航栏（Home/Back/Recent）
└──────────────────────┘
```

### 1.3 当前桌面配置

| 配置项 | 当前值 | 文件位置 |
|---|---|---|
| 背景颜色 | `0x1A1A1A`（深灰） | `stylesheet/core_data.hpp:13` |
| 壁纸图片 | `NULL`（纯色背景） | `stylesheet/core_data.hpp:18` |
| 图标网格列数 | 3 | `stylesheet/app_launcher_data.hpp:33` |
| 图标显示尺寸 | 120×120px | `stylesheet/app_launcher_data.hpp:18` |
| 图标按下尺寸 | 110×110px | `stylesheet/app_launcher_data.hpp:19` |
| 图标标签字体 | 16px | `stylesheet/app_launcher_data.hpp:22` |
| 图标标签颜色 | `0xFFFFFF`（白色） | `stylesheet/app_launcher_data.hpp:23` |
| 图标源数据尺寸 | 112×112px | brookesia 内置图标 |

### 1.4 已安装 App

| App | 名称 | 图标 | 图标文件 | 安装位置 |
|---|---|---|---|---|
| XiaoZhiApp | XiaoZhi | `app_icon_xiaozhi_112_112`（自定义） | `main/xiaozhi_app/app_icon_xiaozhi_112_112.c` | `xiaozhi_app.cc:30` |
| FluidBoxApp | FluidBox | `app_icon_custom_112_112`（自定义） | `components/fluidbox_app/app_icon_custom_112_112.c` | `fluidbox_app.cc:220` |

> **两个 App 均已替换为自定义图标**（v3.8.0 图标定制完成）：
> - **XiaoZhi**：来自"模板 B" 2.png —— 蓝色圆形渐变底座 + 机器人 + 气泡麦克风
> - **FluidBox**：来自"模板 A" 4.png —— 蓝色流体漩涡粒子
> - 均通过连通域泛洪去背景 + 边缘去污染处理（见 §5.4），深色桌面上无光晕

### 1.5 Flash 预算（v3.8.0）

| 分区 | 容量 | 已用 | 剩余 |
|---|---|---|---|
| App (ota_0) | 4,852,224 bytes (4.63MB) | 4,789,072 bytes (4.57MB) | **63,152 bytes (62KB)** |
| Assets | ~6.63MB | 字体/表情/音频 | 较充裕 |

> **注意**：App 分区仅剩 62KB。两个自定义图标共占 ~98KB 源数据。
> 再增加 1 个自定义图标（49KB）可能超出分区，需考虑将图标放入 Assets 分区或优化。

---

## 2. 图标技术规格

### 2.1 精确参数

| 参数 | 值 | 说明 |
|---|---|---|
| **像素尺寸** | 112 × 112 | 源数据尺寸，LVGL 会缩放显示到 120×120 |
| **颜色格式** | `LV_COLOR_FORMAT_ARGB8888` | 32位色，每像素 4 字节 |
| **字节序** | B, G, R, A（小端序） | ESP32-S3 为小端架构 |
| **Stride** | 448 bytes | = 112 × 4 |
| **二进制大小** | 50,176 bytes (49KB) | = 112 × 112 × 4 |
| **C 源码大小** | ~246KB | 含格式化字符 |
| **Alpha 通道** | **必须使用** | 图标边缘/背景必须透明 |
| **透明背景** | 必须 | 非图标内容区域 alpha = 0x00 |

### 2.2 C 数组结构（参考内置图标）

```c
// 文件: app_icon_xxx_112_112.c

#include "lvgl.h"

static const uint8_t app_icon_xxx_112_112_map[] = {
    // 每行 112 像素 × 4 字节 = 448 字节
    // 像素字节序: B, G, R, A
    // 透明像素: 0x00,0x00,0x00,0x00
    // 白色不透明: 0xff,0xff,0xff,0xff
    0x00,0x00,0x00,0x00,  // pixel 0: transparent
    ...
};

const lv_image_dsc_t app_icon_xxx_112_112 = {
  .header.magic = LV_IMAGE_HEADER_MAGIC,
  .header.cf = LV_COLOR_FORMAT_ARGB8888,
  .header.flags = 0,
  .header.w = 112,
  .header.h = 112,
  .header.stride = 448,
  .data_size = sizeof(app_icon_xxx_112_112_map),
  .data = app_icon_xxx_112_112_map,
};
```

### 2.3 在 App 中使用图标

```cpp
// 1. 声明图标
LV_IMAGE_DECLARE(app_icon_xxx_112_112);

// 2. 构造函数中传入
MyApp::MyApp()
    : phone::App("MyApp", &app_icon_xxx_112_112,
                  true,   // use_default_screen
                  true,   // use_status_bar
                  false)  // use_navigation_bar
{}
```

---

## 3. 图标设计要求

### 3.1 透明性（关键！）

**图标必须有透明背景。** Phone Shell 桌面背景色为 `0x1A1A1A`，图标叠加在上面。

| 效果 | 正确做法 | 错误做法 |
|---|---|---|
| 圆形图标 | 圆内不透明，圆外 alpha=0 | 整个方形不透明 |
| 圆角矩形 | 圆角内不透明，角外 alpha=0 | 直角方形 |
| 不规则形状 | 形状内不透明，形状外 alpha=0 | 填充背景色模拟 |

> **⚠️ 伪透明警告**：AI 工具（元宝等）声称的"透明背景"通常不是真透明——
> 输出是 RGB 模式（无 alpha 通道），"透明"只是**烘焙进像素的棋盘格图案**。
> 生成后必须用 §4.5 验证脚本确认真透明（RGBA + alpha<255）。
> **推荐路线：要求纯黑背景生成，再脚本去背景**（详见 §4.1）。

**为什么不能用背景色填充？**
- 背景色可能被修改（更换主题/壁纸）
- 填充色与实际背景有色差
- 缩放时边缘出现锯齿

### 3.2 视觉规格

| 要素 | 建议 |
|---|---|
| **安全区域** | 中央 96×96px（留 8px 边距，按下缩放不裁切） |
| **最小笔画** | ≥ 3px（112px 画布上清晰可见） |
| **风格** | 扁平化/简约，与 Phone Shell 深色主题协调 |
| **配色** | 深色背景上的高对比度色彩 |
| **避免** | 细密纹理、小字、照片级细节（112px 看不清） |
| **避免** | 水印、生成标识（如 "AI生成" 字样） |

### 3.3 常见图标形状

```
圆形（推荐）           圆角方形              自定义形状
┌────────────┐       ┌────────────┐       ┌────────────┐
│   ┌──────┐ │       │ ╭────────╮ │       │    ╱╲╱╲    │
│   │ ICON │ │       │ │  ICON  │ │       │   < ICON >  │
│   │      │ │       │ │        │ │       │    ╲╱╲╱    │
│   └──────┘ │       │ ╰────────╯ │       │            │
│ (透明背景)  │       │ (透明圆角)  │       │ (透明背景)  │
└────────────┘       └────────────┘       └────────────┘
```

---

## 4. 文生图 Prompt（图标生成）

### 4.1 Prompt 设计原则（重要更新）

> **⚠️ 实测结论（v2）**：主流 AI 文生图工具（元宝/通义/DALL-E/Midjourney）的
> "透明背景"输出**不可靠**。实测元宝生成的"透明背景版" PNG 实际为 **RGB 模式（无 alpha 通道）**，
> "透明"只是**烘焙进像素的棋盘格图案**（角落像素如 236,235,233 的浅灰），并非真 alpha=0。
> 工具导出的 C 文件同样全 alpha=255（不透明）。
>
> **因此：默认改用「纯黑背景 + 脚本去背景」路线（实测有效），透明背景仅作为补充尝试。**

**推荐路线（按优先级）：**

| 优先级 | 路线 | 说明 | 实测 |
|---|---|---|---|
| 🥇 **首选** | **纯黑背景 + Python 去背景** | Prompt 要求纯黑背景，生成后用 §5.2 脚本把黑色转透明 | ✅ 已验证（4.png 黑底效果最佳） |
| 🥈 备选 | 工具自带"透明背景"设置 | 仅当工具明确支持真 alpha 输出 | ⚠️ 元宝实测无效，需 §4.5 验证 |
| 🥉 兜底 | 任意背景 + remove.bg 抠图 | 后期 AI 抠图 | ✅ 通用但需额外步骤 |

**设计原则：**
1. **要求纯黑背景**（首选路线）：`Solid pure black background (#000000)` —— 黑底与图标对比强，脚本按亮度阈值去背景最可靠
2. **指定方形画布**：生成 1024×1024 或 512×512（之后缩放到 112×112）
3. **描述简洁明了**：图标在 112px 下需辨识度高
4. **深色主题适配**：图标要在深色背景（#1A1A1A）上醒目

### 4.2 哪些 AI 工具支持透明背景（实测修正）

| 工具 | 真实透明输出 | 实测/依据 |
|---|---|---|
| **元宝 (Qwen)** | ❌ **不可靠** | **实测**：声称"透明背景"但输出 RGB 无 alpha，棋盘格被烘焙进像素 |
| **通义万相** | ⚠️ 待验证 | 声称支持，需用 §4.5 验证脚本确认 |
| **DALL-E 3 (ChatGPT)** | ❌ 不支持 | 生成纯色背景后需后期抠图 |
| **Midjourney** | ❌ 不支持 | 生成纯色背景后需后期抠图 |
| **Stable Diffusion** | ✅ 支持 | 使用 LayerDiffuse 等透明图层插件 |
| **Adobe Firefly** | ⚠️ 待验证 | 声称支持，需验证 |
| **remove.bg / Photoroom** | ✅ 后处理 | 对任意 AI 图自动去背景 |

> **结论**：不要轻信工具的"透明背景"选项。**统一按纯黑背景生成，再脚本去背景**，
> 是最可控的路线（实测成功）。若工具真的输出了 RGBA 且含 alpha<255 像素（§4.5 验证通过），
> 则可直接使用其透明背景。

### 4.3 图标文生图 Prompt 模板

> **⚠️ 以下 Prompt 依据 `xiaozhi_app.cc` / `fluidbox_app.cc` 的实际功能与配色编写**，
> 图标内容应与 App 的真实行为一致，避免文不对题。
>
> **⚠️ 全部改为「纯黑背景版」为默认**（实测可靠）。工具输出后统一走 §5.2 去背景流程。

#### 模板 A：FluidBox（流体模拟）— 纯黑背景版（推荐）

**App 实际功能**（依据 `fluidbox_app.cc` + `render.c`）：
- 实时流体粒子模拟，IMU 重力感应驱动
- 渲染配色：深蓝 `(10,45,165)` → 中蓝 `(40,125,235)` → 亮蓝 `(150,205,250)` → 白 `(255,255,255)`
- 黑色背景上绘制蓝色渐变粒子

```
App icon design, 1024x1024, centered composition with 10% margin.
A dynamic swirl of liquid particles, like fluid simulation,
color gradient from deep blue (#0A2DA5) through bright blue (#96CDFA) to white highlights,
glowing particle dots, glossy 3D effect, flowing motion.
Solid pure black background (#000000), high contrast with the icon.
The icon must be clearly separated from the black background,
no dark elements blending into the background.
Modern, minimalist, app launcher icon style.
No text, no watermark, no border, no frame.
```

> **生成后处理**：用 §5.2 脚本去除黑色背景（亮度<12 → 透明），
> 再按内容裁剪、缩放 112×112、生成 C 数组。

#### 模板 B：XiaoZhi（AI 语音对话助手）— 纯黑背景版（推荐）

**App 实际功能**（依据 `xiaozhi_app.cc`）：
- AI 语音对话（聊天气泡：用户=蓝 #2196F3，助手=绿 #4CAF50，系统=灰 #607D8B）
- 表情显示 + 音频状态符号（LV_SYMBOL_AUDIO）
- 深蓝背景 #1A1A2E

```
App icon design, 1024x1024, centered composition.
A friendly robot head with a speech bubble beside it,
glowing blue (#2196F3) eyes, subtle green (#4CAF50) accent light,
microphone symbol hinting at voice chat.
Solid pure black background (#000000), high contrast with the icon.
Flat design, clean lines, friendly appearance,
suitable for 112x112 downscale.
No text, no watermark, no border.
```

#### 模板 C：通用工具风格 — 纯黑背景版（推荐）

```
App icon design, 1024x1024, centered, simple and bold.
A [描述你的图标内容] symbol,
[主色调] with subtle gradient,
glossy finish, drop shadow.
Solid pure black background (#000000), high contrast with the icon.
The icon must be clearly separated from the black background,
no dark elements blending into the background.
Minimal detail, recognizable at small size (112px).
No text, no watermark, no border, no frame.
```

#### 模板 D：透明背景版（仅当工具确认真 alpha 输出）

> ⚠️ 仅当 §4.5 验证确认工具输出 RGBA 且含 alpha<255 像素时使用。
> **元宝实测此路线无效**（输出 RGB 无 alpha），使用前务必先验证。

```
App icon design, 1024x1024, centered composition with 10% margin.
A [描述你的图标内容],
[主色调] with subtle gradient, glossy finish.
Transparent background, PNG with alpha channel.
Minimal detail, recognizable at small size (112px).
No text, no watermark, no border, no background fill.
```

### 4.4 Prompt 关键词清单

**必须包含（纯黑背景路线）：**
- `App icon design` — 指定图标类型
- `1024x1024` — 指定分辨率
- `centered composition` — 居中构图
- **`Solid pure black background (#000000)`** — 首选路线，便于脚本去背景
- **`The icon must be clearly separated from the black background, no dark elements blending into the background`** — 防止图标暗部与黑底融合（关键！）
- `No text, no watermark, no border` — 避免水印/边框干扰

**可选（透明背景路线，需验证）：**
- ~~`Transparent background, PNG with alpha channel`~~ — 仅当工具确认真 alpha 输出（§4.5 验证）

**推荐包含：**
- `minimalist` / `clean lines` — 简洁，缩小后清晰
- `high contrast` — 深色背景上醒目
- `glossy` / `flat design` — 指定视觉风格
- `recognizable at small size` — 提醒模型保持简单

**避免使用：**
- `photorealistic` — 照片级细节在 112px 下看不清
- `intricate details` / `fine texture` — 细密纹理缩小后糊掉
- `text "XXX"` — AI 生成的文字通常有误
- ~~`gray background`~~ / ~~`white background`~~ — 灰色/白色背景与图标颜色混合，抠图困难
- ~~`colored background`~~ — 彩色背景会增加抠图难度
- ~~`checkerboard`~~ — 不要要求棋盘格背景（那是伪透明的烘焙图案）

### 4.5 透明背景验证方法（生成后必做）

**第一步：检查 PNG 是否为真透明（RGBA + alpha<255）**

```bash
# Python 验证：mode 是否为 RGBA？是否有 alpha<255 像素？
python3 -c "
from PIL import Image
img = Image.open('your_icon.png')
print(f'Mode: {img.mode}')   # RGBA=真透明, RGB=无alpha(伪透明)
alphas = set(px[3] for px in img.convert('RGBA').getdata())
if img.mode == 'RGBA' and len(alphas) > 1:
    print(f'✅ 真透明: alpha 种类 {len(alphas)}, 含 <255 的 {sum(1 for a in alphas if a<255)} 种')
else:
    print('❌ 伪透明或无透明: mode=' + img.mode + ' (RGB=无alpha通道)')
    print('   ⚠️ 元宝/多数工具声称透明背景实际是烘焙棋盘格, 非真alpha!')
    print('   → 改用纯黑背景路线重新生成, 或走§5.2去背景流程')
"
```

**第二步：识别伪透明（烘焙棋盘格）**

```bash
# 检查角落像素：真透明的角落应 alpha=0；伪透明的角落是浅灰/白色图案
python3 -c "
from PIL import Image
img = Image.open('your_icon.png').convert('RGBA')
print(f'TL={img.getpixel((0,0))} TR={img.getpixel((img.width-1,0))}')
print(f'BL={img.getpixel((0,img.height-1))} BR={img.getpixel((img.width-1,img.height-1))}')
# 真透明: alpha=0 (第四个值=0)
# 伪透明(棋盘格): alpha=255 且 RGB 是浅灰(如 236,235,233)
"
```

**判定规则：**
| 角落像素 | 结论 |
|---|---|
| alpha=0（第四个值 0） | ✅ 真透明 |
| alpha=255 + RGB 浅灰（~230-250） | ❌ 伪透明（棋盘格烘焙） |
| alpha=255 + RGB 纯黑（~0-10） | 黑底路线，走 §5.2 去背景 |

**第三步：检查工具导出的 C 文件（若用工具导出）**

```bash
# 检查 C 文件 header：尺寸/格式/变量名/stride 四项
grep -E "\.header\.(cf|w|h|stride)|lv_image_dsc_t" your.c
# 期望:
#   .header.cf = LV_COLOR_FORMAT_ARGB8888,  (图标)
#   .header.w = 112,  .header.h = 112,  .header.stride = 448,
# 常见问题:
#   ❌ w/h 不是 112 (工具默认导出原始尺寸如 1536)
#   ❌ 变量名以数字开头或含连字符 (如 "1"、"c63b4da0-...") — C 非法标识符
#   ❌ 无 .header.stride 字段
#   ❌ 全 alpha=255 (伪透明)
```

---

## 5. 图标后期处理流程

### 5.1 从 AI 生成的 PNG 到可用 C 文件

**⚠️ 重要**：若你让 AI 工具直接导出 C 文件（如元宝），务必先做 §4.5 第三步的
C 文件检查——实测工具导出存在 4 个问题（尺寸 1536 非 112、变量名 `1` 非法、
无 stride、全 alpha=255）。**推荐不要用工具导出的 C 文件，改用以下流程从 PNG 生成。**

```
AI生成PNG（纯黑背景，1024×1024）
    │
    ▼ ① 去背景
    │   - 将接近纯黑的像素（亮度<12）alpha 设为 0
    │   - 保留图标主体不透明
    │   工具: §5.2 Python 脚本 / Photoshop / remove.bg
    │
    ▼ ② 内容裁剪（新）
    │   - 按 alpha>10 的内容包围盒裁剪，去除死边距
    │   - 将图标居中放到正方形画布
    │   工具: §5.2 Python 脚本（自动完成）
    │
    ▼ ③ 检查水印（新）
    │   - 右下角扫描高亮像素簇（白色"AI生成"水印）
    │   - 有水印则裁剪/遮盖/换图
    │   工具: §4.5 方法 / 手动检查
    │
    ▼ ④ 裁剪为圆形/圆角（可选）
    │   - 按目标形状创建 mask
    │   - mask 外 alpha = 0
    │
    ▼ ⑤ 缩放到 112×112
    │   - 使用 LANCZOS 抗锯齿
    │   - 确保缩放后边缘平滑
    │
    ▼ ⑥ 转换为 C 数组
    │   - 格式: LV_COLOR_FORMAT_ARGB8888
    │   - 字节序: B, G, R, A
    │   - stride: 448
    │   工具: §5.2 Python 脚本（自动完成）
    │
    ▼ ⑦ 集成到固件
        - 放入 components/fluidbox_app/
        - CMakeLists.txt 添加 SRCS
        - fluidbox_app.cc 引用
```

### 5.2 自动化处理脚本（Python）

以下脚本完成 ①②③④⑤⑥ 全流程（去背景 → 内容裁剪 → 水印检测 → 形状裁剪 → 缩放 → C 数组）：

```python
#!/usr/bin/env python3
"""
图标处理工具：PNG → 去背景 → 内容裁剪 → 水印检测 → 圆形裁剪 → 缩放 → C 数组
用法: python3 icon_processor.py input.png output_name [shape]
  shape: circle(默认) | rounded | square
输出: output_name.c (112×112 ARGB8888)
"""
from PIL import Image, ImageDraw
import sys, os

def remove_black_bg(img, threshold=12):
    """① 去背景：将接近纯黑的像素设为透明"""
    data = list(img.getdata())
    new_data = []
    for r, g, b, a in data:
        brightness = (r + g + b) / 3
        if brightness < threshold and a > 0:
            new_data.append((0, 0, 0, 0))
        else:
            new_data.append((r, g, b, a))
    img.putdata(new_data)
    return img

def content_crop(img, pad=8):
    """② 内容裁剪：按 alpha>10 的包围盒裁剪，去除死边距"""
    import numpy as np
    arr = np.array(img)[:, :, 3]  # alpha channel
    ys, xs = np.where(arr > 10)
    if len(ys) == 0:
        return img
    y0, y1 = max(ys.min() - pad, 0), min(ys.max() + pad, img.height)
    x0, x1 = max(xs.min() - pad, 0), min(xs.max() + pad, img.width)
    return img.crop((x0, y0, x1, y1))

def center_on_square(img):
    """② 居中到正方形画布"""
    side = max(img.size)
    sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    sq.paste(img, ((side - img.width) // 2, (side - img.height) // 2))
    return sq

def check_watermark(img):
    """③ 水印检测：比较四角高亮像素的相对差异（AI 水印惯例在右下角且一角异常多）"""
    import numpy as np
    arr = np.array(img.convert("L"))
    h, w = arr.shape
    quads = {
        "TL": arr[:h//4, :w//4], "TR": arr[:h//4, 3*w//4:],
        "BL": arr[3*h//4:, :w//4], "BR": arr[3*h//4:, 3*w//4:],
    }
    counts = {name: int(np.sum(q > 200)) for name, q in quads.items()}
    median = sorted(counts.values())[1]  # 中间值作为基准
    for name, count in counts.items():
        # 一角高亮像素远超其他角(>3x 中间值)且超过 2000 → 疑似水印
        if count > max(2000, median * 3):
            print(f"⚠️  检测到 {name} 角 {count} 高亮像素 (其他角约 {median}), 疑似水印!")
            print("   建议: 换图, 或用 PS 修复画笔去除水印")
    print(f"   四角高亮分布: {counts}")

def apply_shape(img, shape):
    """④ 按形状裁剪"""
    if shape == "square":
        return img
    w, h = img.size
    mask = Image.new("L", (w, h), 0)
    draw = ImageDraw.Draw(mask)
    if shape == "circle":
        m = int(min(w, h) * 0.02)
        draw.ellipse([m, m, w-1-m, h-1-m], fill=255)
    elif shape == "rounded":
        r_rad = int(min(w, h) * 0.22)
        draw.rounded_rectangle([0, 0, w-1, h-1], radius=r_rad, fill=255)
    r, g, b, a = img.split()
    a = Image.composite(Image.new("L", (w, h), 255), a, mask)
    return Image.merge("RGBA", (r, g, b, a))

def process_icon(input_png, var_name, shape="circle"):
    SIZE = 112

    # ① 打开并转 RGBA
    img = Image.open(input_png).convert("RGBA")
    print(f"源图: {img.size}, mode={img.mode}")

    # ② 去背景（纯黑背景路线）
    img = remove_black_bg(img)

    # ③ 内容裁剪 + 居中到正方形（去除死边距, 放大图标主体）
    img = content_crop(img)
    img = center_on_square(img)
    print(f"内容裁剪后: {img.size}")

    # ④ 水印检测
    check_watermark(img)

    # ⑤ 形状裁剪
    img = apply_shape(img, shape)

    # ⑥ 缩放到 112×112（LANCZOS 抗锯齿）
    img = img.resize((SIZE, SIZE), Image.LANCZOS)

    # ⑦ 生成 C 数组（LVGL ARGB8888，字节序 B,G,R,A）
    pixels = list(img.getdata())
    lines = ['\n#include "lvgl.h"\n\n']
    lines.append(f'static const uint8_t {var_name}_map[] = {{\n')
    for row in range(SIZE):
        row_bytes = []
        for col in range(SIZE):
            r, g, b, a = pixels[row * SIZE + col]
            row_bytes.append(f"0x{b:02x},0x{g:02x},0x{r:02x},0x{a:02x},")
        lines.append("    " + "".join(row_bytes) + "\n")
    lines.append('};\n\n')
    lines.append(f'const lv_image_dsc_t {var_name} = {{\n')
    lines.append('  .header.magic = LV_IMAGE_HEADER_MAGIC,\n')
    lines.append('  .header.cf = LV_COLOR_FORMAT_ARGB8888,\n')
    lines.append('  .header.flags = 0,\n')
    lines.append(f'  .header.w = {SIZE},\n')
    lines.append(f'  .header.h = {SIZE},\n')
    lines.append(f'  .header.stride = {SIZE * 4},\n')
    lines.append(f'  .data_size = sizeof({var_name}_map),\n')
    lines.append(f'  .data = {var_name}_map,\n')
    lines.append('};\n')

    out_path = f"{var_name}.c"
    with open(out_path, "w") as f:
        f.writelines(lines)

    # ⑧ 验证透明性
    alphas = set(px[3] for px in pixels)
    file_size = os.path.getsize(out_path)
    print(f"✅ Generated: {out_path} ({SIZE}×{SIZE} ARGB8888, {file_size:,} bytes)")
    if len(alphas) == 1 and 255 in alphas:
        print("⚠️  WARNING: 所有像素不透明(alpha=255)！")
        print("   去背景未生效。可能原因:")
        print("   1) 源图背景不是纯黑(如浅灰棋盘格伪透明) → 需换纯黑背景图")
        print("   2) 图标本身充满全画面 → 用 circle/rounded 裁剪")
    else:
        trans = sum(1 for a in alphas if a < 255)
        print(f"✅ 有透明区域, alpha 值种类: {len(alphas)} ({trans} 种 < 255)")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 icon_processor.py input.png output_name [circle|rounded|square]")
        sys.exit(1)
    shape = sys.argv[3] if len(sys.argv) > 3 else "circle"
    process_icon(sys.argv[1], sys.argv[2], shape)
```

### 5.3 手动处理（Photoshop / GIMP）

1. **打开** AI 生成的 PNG
2. **魔棒工具** 选中背景区域 → 删除（变为透明）
3. **如有水印文字**：用修复画笔/克隆工具移除
4. **裁剪为正方形**（保留图标居中）
5. **图像大小** → 112×112 像素（重采样：Lanczos）
6. **导出为 PNG**（保留透明通道）
7. 用 Python 脚本或 LVGL Image Converter 转为 C 数组

### 5.4 浅色背景去背景方案（XiaoZhi 图标实测）

> **适用场景**：主体是**浅色**（白/灰机器人）+ 背景是**浅色棋盘格**（伪透明）。
> 亮度阈值去背景失效（主体与背景亮度接近），必须用**连通域泛洪**。

**核心原理**：背景是与图片四边**空间相连**的"类白"区域；白色主体被彩色边缘
（蓝眼/绿光/阴影/轮廓）包围，形成**不与边界相连的孤立连通域**。
用空间连通性而非颜色相似性区分主体与背景。

**算法**（`scipy.ndimage`，无 OpenCV 依赖）：

```python
import numpy as np
from scipy import ndimage

def remove_light_bg(img_rgb, threshold=228):
    # 1. 类背景 mask：全通道 ≥ 阈值（覆盖棋盘格 238-252，排除主体阴影 <225）
    bg_like = np.all(img_rgb >= threshold, axis=2)
    # 2. 连通域标记
    labeled, _ = ndimage.label(bg_like)
    # 3. 收集触及四边的标签 = 背景
    border = set()
    for edge in [labeled[0,:], labeled[-1,:], labeled[:,0], labeled[:,-1]]:
        border.update(edge.tolist())
    border.discard(0)
    # 4. 背景 mask + 膨胀 1px（防边缘渗入）
    bg = ndimage.binary_dilation(np.isin(labeled, list(border)), iterations=1)
    # 5. 主体 = 非背景，形态学清理
    subject = ~bg
    subject = ndimage.binary_fill_holes(subject)
    subject = ndimage.binary_opening(subject, structure=np.ones((3,3)), iterations=1)
    subject = ndimage.binary_fill_holes(subject)
    return subject.astype(np.uint8) * 255  # 255=主体, 0=背景
```

**边缘颜色去污染**（防深色桌面光晕）：

```python
def decontaminate(rgb, alpha, bg_color):
    out = rgb.astype(float).copy()
    cov = alpha.astype(float) / 255.0
    edge = (cov > 0.01) & (cov < 0.99)
    for c in range(3):
        rec = (out[:,:,c] - bg_color[c] * (1 - cov)) / np.maximum(cov, 0.01)
        out[edge, c] = np.clip(rec[edge], 0, 255)
    return out.astype(np.uint8)
```

**完整流水线**（`remove_checker_bg.py`）：
```
PNG (1536×1536 RGB 无alpha)
  → ① 连通域泛洪去背景（threshold=228）
  → ② 形态学清理（fill_holes / opening）
  → ③ 内容裁剪 + 居中到正方形（bg_color 填充）
  → ④ LANCZOS 缩放 112×112
  → ⑤ 边缘颜色去污染
  → ⑥ LVGLImage.py → ARGB8888 C 数组
```

**调参指引**：
| 现象 | 调整 |
|---|---|
| 背景未完全去除 | 提高 `threshold` 至 232 |
| 主体被误删（白色部分） | 降低 `threshold` 至 220 |
| 边缘光晕 | 增大 `EDGE_ERODE` 或设置 `ERODE_FINAL=1` |
| 主体延伸到图片边缘 | 保证图标有边距；或降低阈值排除主体边缘像素 |

> **实测结果**（XiaoZhi 模板 B 五张图）：
> 5 张全部处理成功，深色背景（0x1A1A1A）渲染**无光晕**。
> 视觉质量排名：2 > 3 > 5 > 1 > 4，最终选中 2.png（蓝底机器人）。

---

## 6. 壁纸/桌面背景定制

### 6.1 纯色背景（当前方式）

修改 `stylesheet/core_data.hpp` 第 13 行：

```cpp
// 当前值
constexpr uint32_t STYLESHEET_410_502_DARK_CORE_DISPLAY_BG_COLOR = 0x1A1A1A;

// 常见替代色
// 0x000000  纯黑（OLED 省电）
// 0x0A0A23  深蓝
// 0x1A0A2E  深紫
// 0x0D1117  GitHub Dark
```

### 6.2 壁纸技术规格

**关键发现**：brookesia 内置壁纸使用 `LV_COLOR_FORMAT_RGB888`（3 bytes/像素，无 alpha 通道）。
已验证 `esp_brookesia_image_middle_wallpaper_dark_480_480.c`：
- 格式：`LV_COLOR_FORMAT_RGB888`
- 无 `.header.flags`、无 `.header.stride` 字段（旧版 header）
- 实际内容为纯色填充（全部 `0x1a` = `#1A1A1A`）

| 格式 | 410×502 二进制大小 | C 源码大小 | 说明 |
|---|---|---|---|
| **RGB888（3字节/px）** | 611,436 bytes (597KB) | ~3.2MB | ✅ brookesia 实际使用格式 |
| RGB565（2字节/px） | 411,640 bytes (402KB) | ~2.1MB | ❌ 与 brookesia 不兼容 |
| ARGB8888（4字节/px） | 823,280 bytes (804KB) | ~4.3MB | ❌ 壁纸不需要 alpha，浪费空间 |
| 压缩 JPEG | ~30-80KB | — | 需 LVGL JPEG 解码支持 |

**C 数组结构（RGB888，与 brookesia 一致）**：

```c
static const uint8_t wallpaper_410_502_map[] = {
    // 每行 410 像素 × 3 字节 = 1230 字节
    // 像素字节序: R, G, B（RGB888 无 alpha）
    0x1a,0x1a,0x1a,  // pixel 0
    ...
};

const lv_image_dsc_t wallpaper_410_502 = {
  .header.magic = LV_IMAGE_HEADER_MAGIC,
  .header.cf = LV_COLOR_FORMAT_RGB888,
  .header.w = 410,
  .header.h = 502,
  .data_size = sizeof(wallpaper_410_502_map),
  .data = wallpaper_410_502_map,
};
```

> **注意**：RGB888 的字节序是 **R, G, B**（与 ARGB8888 图标的 B,G,R,A 不同）。

### 6.3 图片壁纸可行性（Flash 约束）

| 方案 | 空间需求 | 可行性 |
|---|---|---|
| App 分区嵌入（当前） | 597KB | ❌ 仅剩 64KB，完全放不下 |
| Assets 分区（6.63MB） | 597KB | ✅ 空间足够，但需开发 assets 加载逻辑 |
| 纯色背景 | 0KB | ✅ 当前方案 |
| LVGL 渐变背景 | ~0KB（代码绘制） | ✅ 推荐，见 §6.5 |

> **结论**：410×502 RGB888 壁纸 597KB 远超 App 分区剩余空间（64KB）。
> 短期内推荐纯色背景或代码渐变；若要图片壁纸，需把壁纸放入 Assets 分区并开发加载逻辑。

### 6.4 壁纸设计规范

**壁纸与图标不同——必须不透明，且不能干扰图标辨识：**

| 要素 | 要求 |
|---|---|
| **尺寸** | 410×502（屏幕物理分辨率），或 2 倍 820×1004 再缩放 |
| **颜色格式** | RGB888（3 字节/像素，无 alpha） |
| **明度** | 深色为主，平均亮度低（图标是浅色，需背景衬托） |
| **对比度** | 图标区（网格区域）保持低对比度，避免与图标撞色 |
| **图案密度** | 大面积平滑渐变，避免密集纹理/文字 |
| **避免** | 高亮中心、与图标同色的图案、细密条纹（摩尔纹） |
| **主题协调** | 与 Phone Shell 深色主题（#1A1A1A）协调 |

**推荐配色方向**（暗色调 + 微渐变）：
- 深蓝渐变：`#0A0A23` → `#16213E`
- 深紫渐变：`#1A0A2E` → `#2D1B4E`
- 深灰渐变：`#1A1A1A` → `#2A2A2A`

### 6.5 壁纸文生图 Prompt（如走 Assets 分区方案）

> 壁纸生成时**不需要透明背景**（它是背景本身），生成后转为 RGB888。
> 注意：壁纸不能放进 App 分区（见 §6.3），以下 Prompt 仅供 Assets 方案使用。

```
Phone wallpaper, 820x1004 (2x), vertical orientation.
Dark abstract gradient background, deep navy blue (#0A0A23) to dark slate (#16213E),
subtle soft glow in the lower third, smooth color transition, no sharp edges.
Very dark, low contrast, minimalist, suitable as app launcher backdrop.
No text, no logos, no icons, no watermarks, no busy patterns.
Keep the center area plain dark for icon visibility.
```

```
Phone wallpaper, 820x1004 (2x), vertical orientation.
Dark purple gradient, #1A0A2E to #2D1B4E, with a faint nebula-like soft haze,
extremely subtle, low brightness, no stars, no particles, no text.
Minimalist dark theme wallpaper, smooth gradients only.
No text, no logos, no icons, no watermarks.
```

### 6.6 壁纸设置方法

```cpp
// stylesheet/core_data.hpp
constexpr base::Display::Data STYLESHEET_410_502_DARK_CORE_DISPLAY_DATA = {
    .background = {
        .color = gui::StyleColor::COLOR(0x1A1A1A),
        .wallpaper_image_resource = NULL,  // ← 改为 gui::StyleImage::IMAGE(&wallpaper_410_502)
    },
    ...
};
```

### 6.7 渐变背景（推荐，无需图片）

不占 Flash 的代码渐变方案——在 `core_data.hpp` 颜色不变的前提下，
于 `brookesia_display.cc` 或 App Launcher 绘制层叠渐变：

```cpp
// 在 Phone Shell 背景上叠加渐变（需要挂载在 launcher 容器）
lv_obj_t* grad = lv_obj_create(lv_layer_top());
lv_obj_set_size(grad, 410, 502);
lv_obj_set_style_bg_opa(grad, LV_OPA_COVER, 0);
lv_obj_set_style_bg_grad_color(grad, lv_color_hex(0x0A0A23), 0);
lv_obj_set_style_bg_color(grad, lv_color_hex(0x1A1A2E), 0);
lv_obj_set_style_bg_grad_dir(grad, LV_GRAD_DIR_VER, 0);
// 置于图标层之下
lv_obj_move_background(grad);
```

> ⚠️ 此方案为示意，实际需要确认 App Launcher 的容器层级，避免覆盖图标。
> 若层级控制复杂，直接修改 `STYLESHEET_410_502_DARK_CORE_DISPLAY_BG_COLOR` 换纯色更稳妥。

---

## 7. 完整集成工作流

### 7.1 从零开始：生成 → 处理 → 集成 → 编译

```
Step 1: 生成图标图片
  └─ 用 AI 文生图工具（元宝/通义/DALL-E/Midjourney）按 §4 的 Prompt 生成
  └─ 输出: my_icon_1024.png

Step 2: 后期处理
  └─ 去背景（黑色 → 透明）
  └─ 移除水印文字
  └─ 缩放到 112×112
  └─ 输出: my_icon_112.png

Step 3: 转换为 C 数组
  └─ 使用 Python 脚本（§5.2）或 LVGL Image Converter
  └─ 格式: LV_COLOR_FORMAT_ARGB8888
  └─ 输出: app_icon_my_icon_112_112.c

Step 4: 集成到项目
  └─ 复制 .c 文件到 components/fluidbox_app/
  └─ 编辑 CMakeLists.txt，SRCS 加入文件名
  └─ 编辑 fluidbox_app.cc:
       LV_IMAGE_DECLARE(app_icon_my_icon_112_112);
       构造函数中: &app_icon_my_icon_112_112

Step 5: 编译验证
  └─ idf.py build
  └─ 检查 binary size 是否超出分区

Step 6: 烧录测试
  └─ idf.py flash
  └─ 检查桌面图标显示是否正确
```

### 7.2 验证清单

**图标：**
- [ ] C 文件 header 声明 `LV_COLOR_FORMAT_ARGB8888`（非 RGB565）
- [ ] 尺寸 w=112, h=112, stride=448
- [ ] 变量名为合法 C 标识符（下划线，不含连字符）
- [ ] 四角像素 alpha = 0x00（透明）
- [ ] CMakeLists.txt 已添加 SRCS
- [ ] `LV_IMAGE_DECLARE` 已添加到 .cc 文件
- [ ] 构造函数引用正确的图标变量
- [ ] `idf.py build` 编译通过
- [ ] binary size 未超出分区

**壁纸（仅当走 Assets 方案）：**
- [ ] C 文件 header 声明 `LV_COLOR_FORMAT_RGB888`（非 ARGB8888/RGB565）
- [ ] 尺寸 w=410, h=502，字节序 R,G,B
- [ ] 壁纸未放入 App 分区（597KB 装不下，需 Assets 方案）
- [ ] 背景为深色低对比，不干扰图标辨识

---

## 8. 常见问题与陷阱

### Q1: 图标显示为不透明方块

**原因**：源 PNG 没有真透明通道。两种常见情况：
1. **无 alpha 通道**（mode=RGB，100% alpha=0xFF）
2. **伪透明**：工具声称"透明背景"但实际是**烘焙的棋盘格图案**（浅灰像素，alpha=255）

**解决**：
1. 用 §4.5 验证脚本确认是真透明还是伪透明
2. 真透明 → 直接转换；伪透明/无 alpha → 走纯黑背景路线重新生成，或 §5.2 去背景
3. 参考 §5 图标后期处理流程

### Q2: C 文件过大（几 MB 甚至十几 MB）

**原因**：源图片尺寸不是 112×112（如 1024×1024、1536×1536）。

**数据量计算**：
| 源尺寸 | 二进制 | C 源码 |
|---|---|---|
| 112×112 | 49KB | 246KB |
| 256×256 | 256KB | 1.3MB |
| 512×512 | 1MB | 5.2MB |
| 1024×1024 | 4MB | 20MB |
| 1536×1536 | 9MB | 56MB |

**解决**：先缩放到 112×112，再转 C 数组。
**工具导出陷阱**：AI 工具（如元宝）导出的 C 文件默认是**原始尺寸**（如 1536×1536 = 56MB），
不能直接用。只取工具的 PNG，用 §5.2 脚本重新生成。**不要用工具导出的 C 文件。**

### Q3: 图标颜色格式错误（RGB565 vs ARGB8888）

**症状**：LVGL Image Converter 默认可能选 RGB565（无 alpha）。

**验证**：检查 C 文件 header：
```c
// 图标必须是 ARGB8888（有 alpha，字节序 B,G,R,A）
.header.cf = LV_COLOR_FORMAT_ARGB8888,  // ✅ 正确
.header.cf = LV_COLOR_FORMAT_RGB565,    // ❌ 错误（无透明通道）

// 壁纸必须是 RGB888（无 alpha，字节序 R,G,B）
.header.cf = LV_COLOR_FORMAT_RGB888,    // ✅ 壁纸正确
.header.cf = LV_COLOR_FORMAT_RGB565,    // ❌ 与 brookesia 不兼容
```

> **图标 vs 壁纸格式对比**：
> | 用途 | 格式 | 字节序 | Alpha |
> |---|---|---|---|
> | 图标 | ARGB8888 | B,G,R,A | ✅ 必须 |
> | 壁纸 | RGB888 | R,G,B | ❌ 无 |

### Q4: 编译报错 "undefined reference"

**原因**：CMakeLists.txt 未添加 .c 文件到 SRCS，或变量名不匹配。

**解决**：
1. 确认 .c 文件在 `CMakeLists.txt` 的 `SRCS` 列表中
2. 确认 `LV_IMAGE_DECLARE(变量名)` 中的名称与 C 文件中一致
3. 确认构造函数引用的 `&变量名` 一致

### Q5: 变量名非法导致编译错误

**原因**：AI 工具导出的 C 文件变量名可能：
- 以数字开头（如 `1`、`2_map`）— C 标识符不能以数字开头
- 含连字符/UUID（如 `c63b4da0-9269-...`）— 连字符非法

**解决**：
1. **推荐**：不用工具导出的 C 文件，用 §5.2 脚本从 PNG 重新生成（自动用合法变量名）
2. 若必须用，重命名为合法 C 标识符（仅字母、数字、下划线，字母或下划线开头）

### Q6: Flash 空间不足

**当前状态**：App 分区剩余 62KB（4,852,224 - 4,786,400 = 63,264 bytes），一个 ARGB8888 图标 49KB。

**图标对策**：
- 仅替换必要的图标（每个 49KB，最多再加 1 个）
- 使用 RGB565 格式（24.5KB/图标，但无透明通道）
- 将图标放入 Assets 分区（需额外开发加载逻辑）
- 优化其他代码释放 Flash 空间

**壁纸对策**（更严峻）：
- 410×502 RGB888 壁纸 = 597KB，**远超** 62KB 剩余空间
- 壁纸只能走 Assets 分区方案或代码渐变/纯色（见 §6.3、§6.7）

### Q7: AI 工具导出的 C 文件不能直接用（实测清单）

**实测**：元宝 AI 导出的 `1.c`（1536×1536）存在 5 个问题：

| # | 问题 | 实测值 | 正确值 |
|---|---|---|---|
| 1 | 尺寸过大 | 1536×1536（C 文件 56MB） | 112×112 |
| 2 | 变量名非法 | `1`（数字开头） | `app_icon_xxx_112_112` |
| 3 | 无 stride 字段 | 缺失 | `.header.stride = 448` |
| 4 | 全不透明 | 所有像素 alpha=255 | 背景 alpha=0 |
| 5 | 无 PNG 配套验证 | 工具 C 与 PNG 独立生成 | 从 PNG 重新转换 |

**结论**：**不要用 AI 工具导出的 C 文件**。正确流程：
1. 只用工具生成的 **PNG**（纯黑背景版）
2. 用 §5.2 `icon_processor.py` 从 PNG 重新生成 C 文件
3. 用 §4.5 验证脚本确认最终结果

---

## 9. 当前项目文件索引

| 文件 | 用途 |
|---|---|
| `stylesheet/core_data.hpp` | 背景色、壁纸配置 |
| `stylesheet/app_launcher_data.hpp` | 图标网格布局、图标尺寸 |
| `brookesia_display.cc` | App 安装、Phone Shell 创建 |
| `main/xiaozhi_app/xiaozhi_app.cc` | XiaoZhi App 定义（图标引用 L30） |
| `main/xiaozhi_app/app_icon_xiaozhi_112_112.c` | **XiaoZhi 自定义图标**（蓝底机器人，模板B 2.png） |
| `main/CMakeLists.txt` | XiaoZhi 图标编译配置（L609） |
| `components/fluidbox_app/fluidbox_app.cc` | FluidBox App 定义（图标引用 L220） |
| `components/fluidbox_app/fluidbox_app.h` | FluidBox App 头文件 |
| `components/fluidbox_app/CMakeLists.txt` | FluidBox 组件编译配置 |
| `components/fluidbox_app/app_icon_custom_112_112.c` | **FluidBox 自定义图标**（蓝色漩涡，模板A 4.png） |
| `components/brookesia_core/.../default_112_112.c` | 内置默认图标（参考模板） |

---

## 附录 A: Python 转换脚本汇总

> §5.2 的 `icon_processor.py` 是**完整版**（去背景 + 内容裁剪 + 水印检测 + 形状裁剪 + 缩放 + C 数组 + 透明验证）。
> 本附录补充两个专用场景脚本。

### A1. 图标缩放转换（已有透明 PNG）

```python
#!/usr/bin/env python3
"""
已有透明背景 PNG → LVGL ARGB8888 C Array
用法: python3 png_to_lvgl.py input.png output_name [size]
输出: output_name.c (size×size, ARGB8888, 字节序 B,G,R,A)
"""
from PIL import Image
import sys

def convert(input_png, var_name, size=112):
    img = Image.open(input_png).convert("RGBA")
    img = img.resize((size, size), Image.LANCZOS)
    pixels = list(img.getdata())

    lines = ['\n#include "lvgl.h"\n\n']
    lines.append(f'static const uint8_t {var_name}_map[] = {{\n')

    for row in range(size):
        row_bytes = []
        for col in range(size):
            r, g, b, a = pixels[row * size + col]
            row_bytes.append(f"0x{b:02x},0x{g:02x},0x{r:02x},0x{a:02x},")
        lines.append("    " + "".join(row_bytes) + "\n")

    lines.append('};\n\n')
    lines.append(f'const lv_image_dsc_t {var_name} = {{\n')
    lines.append('  .header.magic = LV_IMAGE_HEADER_MAGIC,\n')
    lines.append('  .header.cf = LV_COLOR_FORMAT_ARGB8888,\n')
    lines.append('  .header.flags = 0,\n')
    lines.append(f'  .header.w = {size},\n')
    lines.append(f'  .header.h = {size},\n')
    lines.append(f'  .header.stride = {size * 4},\n')
    lines.append(f'  .data_size = sizeof({var_name}_map),\n')
    lines.append(f'  .data = {var_name}_map,\n')
    lines.append('};\n')

    output_path = f"{var_name}.c"
    with open(output_path, "w") as f:
        f.writelines(lines)

    alpha_vals = set(px[3] for px in pixels)
    print(f"Generated: {output_path} ({size}×{size} ARGB8888)")
    if len(alpha_vals) == 1 and 255 in alpha_vals:
        print("⚠️  WARNING: 所有像素不透明(alpha=255)! 源图无透明通道。")
        print("   请用 §5.2 完整脚本（含去背景/裁剪）重新处理。")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 png_to_lvgl.py input.png output_name [size]")
        sys.exit(1)
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 112
    convert(sys.argv[1], sys.argv[2], size)
```

### A2. 壁纸转换（RGB888，无 alpha）

```python
#!/usr/bin/env python3
"""
壁纸 PNG → LVGL RGB888 C Array（与 brookesia 内置壁纸格式一致）
用法: python3 png_to_wallpaper.py input.png wallpaper_410_502
输出: wallpaper_410_502.c (RGB888, 字节序 R,G,B)
"""
from PIL import Image
import sys

def convert(input_png, var_name, w=410, h=502):
    img = Image.open(input_png).convert("RGB")  # 丢弃 alpha
    img = img.resize((w, h), Image.LANCZOS)
    pixels = list(img.getdata())

    lines = ['\n#include "lvgl.h"\n\n']
    lines.append(f'static const uint8_t {var_name}_map[] = {{\n')

    for row in range(h):
        row_bytes = []
        for col in range(w):
            r, g, b = pixels[row * w + col]
            row_bytes.append(f"0x{r:02x},0x{g:02x},0x{b:02x},")
        lines.append("    " + "".join(row_bytes) + "\n")

    lines.append('};\n\n')
    lines.append(f'const lv_image_dsc_t {var_name} = {{\n')
    lines.append('  .header.magic = LV_IMAGE_HEADER_MAGIC,\n')
    lines.append('  .header.cf = LV_COLOR_FORMAT_RGB888,\n')
    lines.append(f'  .header.w = {w},\n')
    lines.append(f'  .header.h = {h},\n')
    lines.append(f'  .data_size = sizeof({var_name}_map),\n')
    lines.append(f'  .data = {var_name}_map,\n')
    lines.append('};\n')

    output_path = f"{var_name}.c"
    with open(output_path, "w") as f:
        f.writelines(lines)

    import os
    print(f"Generated: {output_path} ({w}×{h} RGB888, {os.path.getsize(output_path):,} bytes)")
    print(f"⚠️  注意: 壁纸 {os.path.getsize(output_path):,} bytes 无法放入 App 分区(仅剩 64KB)")
    print("    需放入 Assets 分区并开发加载逻辑, 或改用纯色/渐变背景(§6.7)")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 png_to_wallpaper.py input.png output_name [w] [h]")
        sys.exit(1)
    w = int(sys.argv[3]) if len(sys.argv) > 3 else 410
    h = int(sys.argv[4]) if len(sys.argv) > 4 else 502
    convert(sys.argv[1], sys.argv[2], w, h)
```

---

## 附录 B: 图标案例分析报告

### B1. 第一版: `c63b4da0-9269-4654-8248-3eb3c795730d.png`（失败案例）

| 属性 | 值 |
|---|---|
| 尺寸 | 1024 × 1024 |
| 模式 | RGBA |
| Alpha 分布 | 100% = 0xFF（完全不透明） |
| 内容 | 蓝色漩涡/流体抽象图案，黑色圆形边框 |
| 背景 | 灰色实心（无透明） |
| 水印 | 右下角 "元宝助手" / "AI生成" |

**问题清单**：
1. **尺寸过大**：1024×1024（应 112×112）
2. **无透明通道**：100% 像素 alpha=255
3. **有水印文字**："元宝助手" / "AI生成"
4. **灰色背景**：应透明
5. **原始转换格式错误**：RGB565（应 ARGB8888）

**结论**：源图本身不合格（无透明 + 水印 + 灰底），直接转换无法使用。

### B2. 第二版: 模板 A 生成的 4 张图（伪透明失败 → 黑底成功）

| 文件 | 尺寸 | 背景 | 水印扫描 | 结论 |
|---|---|---|---|---|
| 1.png (+1.c) | 1536×1536 | 浅灰棋盘格（烘焙伪透明） | TR 高亮 | ❌ 伪透明，抠图困难 |
| 2.png | 1536×1536 | 纯黑 | TR=8200 疑似水印 | ⚠️ 有水印风险 |
| 3.png | 1536×1536 | 纯白 | 大量高亮 | ❌ 白底抠图困难 |
| **4.png** | 1536×1536 | **纯黑** | **BR=0 无水印** | ✅ **选中** |

**关键教训**：
1. 工具声称"透明背景"实际是**烘焙棋盘格**（RGB 无 alpha）——**不可信**
2. 工具导出的 C 文件（1.c）56MB：1536 尺寸 + 变量名 `1` 非法 + 无 stride + 全 alpha=255 —— **不可用**
3. **纯黑背景 + 亮度阈值去背景**是唯一实测可行的路线

**成功流程（4.png）**：
- 去黑背景（亮度<12 → 透明）→ 内容裁剪（alpha>10 包围盒）→ 正方形画布 → LANCZOS 缩放 112×112 → ARGB8888 C 数组
- 结果：27% 透明像素，角落 alpha=00，中心深蓝漩涡 (8,20,43)
- 已集成到 `components/fluidbox_app/app_icon_custom_112_112.c`，编译通过

### B3. 第三版: 模板 B 生成的 5 张图（浅色背景 → 连通域泛洪成功）

| 文件 | 尺寸 | 背景 | 主体 | 处理 | 结论 |
|---|---|---|---|---|---|
| 1.png | 1536×1536 | 浅灰棋盘格 | 机器人+蓝光晕+气泡 | 连通域去背景 | ❌ 主体不够突出 |
| **2.png** | 1536×1536 | 浅灰棋盘格 | **蓝圆底机器人+气泡麦克风** | 连通域去背景 | ✅ **选中**（视觉最佳） |
| 3.png | 1536×1536 | 浅灰棋盘格 | 机器人+蓝绿波浪圆环 | 连通域去背景 | ⚠️ 次选（真透明） |
| 4.png | 1536×1536 | 纯白 | 机器人+银河大脑 | 连通域去背景 | ❌ 主体偏小 |
| 5.png | 1536×1536 | 浅灰棋盘格 | 白机器人+绿天线 | 连通域去背景 | ⚠️ 简洁但对比弱 |

**关键难点**：主体是**白色机器人** + 背景是**浅灰棋盘格**——亮度阈值去背景失效
（两者亮度接近，245-255 重叠）。必须用 **§5.4 连通域泛洪**（空间连通性，非颜色相似性）。

**成功流程（2.png）**：
- 连通域泛洪去背景（threshold=228）→ 形态学清理 → 内容裁剪居中 → LANCZOS 缩放 112×112 → 边缘去污染 → LVGLImage.py → ARGB8888
- 结果：77% 不透明（蓝底是设计元素），四角 alpha=00，深色背景无光晕
- 已集成到 `main/xiaozhi_app/app_icon_xiaozhi_112_112.c`，编译通过

**最终状态（v3.8.0）**：
| App | 图标 | 来源 | 处理方案 |
|---|---|---|---|
| XiaoZhi | `app_icon_xiaozhi_112_112` | 模板B 2.png | 连通域泛洪 + 边缘去污染 |
| FluidBox | `app_icon_custom_112_112` | 模板A 4.png | 亮度阈值去黑底 |

---

*文档版本: v1.1 | 最后更新: 2025-08-08 | 对应固件版本: v3.8.0*

> v1.1 更新：XiaoZhi + FluidBox 双图标定制完成；新增 §5.4 浅色背景去背景方案（连通域泛洪）；更新已安装 App 表、Flash 预算、文件索引、附录 B3。
