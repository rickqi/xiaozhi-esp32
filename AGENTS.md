# AGENTS.md

本文件为 OpenCode 会话提供仓库专属指引，只记录"不看会踩坑"的高密度信息。

---

## 项目概述

小智 AI 聊天机器人 ESP32 固件（78/xiaozhi-esp32 定制分支），目标板 **waveshare-s3-rlcd-4.2**（ESP32-S3，4.2 寸圆形 ST7305 单色 LCD，ES8311+ES7210 音频，SHTC3 温湿度，PCF85063 RTC，SD 卡）。

---

## 构建环境

| 项 | 值 |
|---|---|
| ESP-IDF | 5.5.2，路径 `C:\Users\szk220009\esp\esp-idf` |
| Python env | `C:\Users\szk220009\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe` |
| 环境激活 | `& C:\Users\szk220009\esp\esp-idf\export.ps1`（PowerShell） |
| 目标芯片 | `esp32s3` |
| 串口 | **COM4**（USB-Serial/JTAG，非 UART 桥接） |

```powershell
# 激活 + 构建
& C:\Users\szk220009\esp\esp-idf\export.ps1
idf.py build
idf.py -p COM4 flash
idf.py -p COM4 monitor   # 或用 pyserial 直接发命令（见下）
```

> **注意**：`idf.py monitor` 会独占 COM4，烧录前必须先终止 monitor 进程。

**串口命令测试**（不阻塞，适合自动化验证）：
```powershell
& "C:\Users\szk220009\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe" -c "
import serial, time
s = serial.Serial('COM4', 115200, timeout=0.3)
time.sleep(0.3); s.read(4096)  # drain
s.write(b'CHATLOGLIST\n'); s.flush(); time.sleep(3); print(s.read(4096).decode('utf-8','replace'))
"
```

---

## Git 与分支

| 分支 | 说明 |
|---|---|
| `master` | **当前工作分支**，定制功能线（v2.3.0），已推送 origin |
| `rebuild-v2.4` | 官方 v2.4.0 基线 + 移植功能，本地实验分支，未推送 |
| `origin/main` / `origin/master` | 远程（origin = `https://github.com/rickqi/xiaozhi-esp32.git`） |

> master 与 rebuild-v2.4 **无共同祖先**，不可直接 merge/cherry-pick。

**提交规范**：`feat:` / `fix:` 前缀 + 中文或英文描述。多行 message 用 `git commit -F <file>` 避免 PowerShell 解析问题（圆括号、反引号会被 shell 误解析）。

---

## 版本管理

**版本号唯一来源**：`CMakeLists.txt:7` 的 `set(PROJECT_VER "...")`，自动注入 `esp_app_desc_t.version`，传播到 OTA / 显示 / MCP / User-Agent。

**Git 提交戳**：`main/CMakeLists.txt` 在构建时执行 `git rev-parse --short HEAD`，通过 `GIT_COMMIT` 宏嵌入固件（失败回退 `"unknown"`）。

### 版本 bump 判断准则

| 改动类型 | 版本 bump | 示例 |
|---|---|---|
| 纯 bug 修复、显示修正 | patch（x.y.**Z**） | C1 通知空闲时钟抢占修复 |
| 新增功能、MCP 工具、新能力 | minor（x.**Y**.0） | chatlog 管理、版本信息工具 |
| 分区变更、协议破坏、不兼容改动 | major（**X**.0.0） | OTA 协议改版、分区表不兼容 |

> 每次提交前判断是否需要 bump 版本；有新功能必须 bump minor。

### 版本更新强制要求

**每次版本 bump 必须同步更新以下内容**（缺一不可）：

1. **`CHANGELOG.md`**：在该文件顶部（最新版本）增加本次变更条目，按 `feat` / `fix` / `change` 分类，简述改了什么
2. **`main/version_info.cc` 的 `kFeatures[]`**：如果新增/删除/变更了功能，同步更新功能清单数组（语音查询 `self.get_version_info` 时播报的就是这个数组）
3. **版本号**：`CMakeLists.txt:7` 的 `set(PROJECT_VER "...")`
4. **文档**：`docs/usage.md` 和板 `README.md` 中受影响的 MCP 工具表 / 串口命令表 / 功能说明

> 纯文档提交（如 AGENTS.md、分析报告）**不需要 bump 版本**，也无需更新 CHANGELOG。

---

## 关键配置（sdkconfig.defaults.esp32s3）

以下配置**缺一不可**，手动编辑 `sdkconfig` 时勿误删：

```ini
# Console：该板无 UART 桥接，必须用 USB-Serial/JTAG
CONFIG_ESP_CONSOLE_UART_DEFAULT=n
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y

# FATFS 中文文件名三件套（缺任一项中文文件名失效）
CONFIG_FATFS_CODEPAGE_936=y
CONFIG_FATFS_API_ENCODING_UTF_8=y
# 另需 CONFIG_FATFS_LFN_HEAP=y（LFN_NONE 会使 API_ENCODING 选项不可见）

# 唤醒词模型
CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y
```

> `sdkconfig` 需手动维护——`idf.py reconfigure` 可能清除手动设置的 Kconfig `choice` 选项。

---

## 分区表

使用 **v2 分区**（`partitions/v2/16m.csv`）：双 OTA 槽 `ota_0`/`ota_1`（各 ~4MB）+ `assets`（SPIFFS，8MB，唤醒词/字体/表情/音频）。

> v1 与 v2 **不兼容**，无法互相 OTA。`main/CMakeLists.txt` 末尾会自动检测 `assets` 分区并注册到 `idf.py flash`。

---

## 代码架构要点

### MCP 工具注册（板级）

所有设备端 MCP 工具在 `main/boards/waveshare-s3-rlcd-4.2/waveshare-s3-rlcd-4.2.cc` 的 `InitializeTools()` 中注册。

| 方法 | AI 可见 | 语音可调用 |
|---|---|---|
| `mcp_server.AddTool("self.xxx", ...)` | ✅ | ✅ |
| `mcp_server.AddUserOnlyTool("self.xxx", ...)` | ❌ | ❌ |

> `self.get_system_info` 是 user-only（AI 不可见）；如需语音查询版本/系统信息，用 `AddTool` 注册的 `self.get_version_info`。

ReturnValue 类型：`std::variant<bool, int, std::string, cJSON*, ImageContent*>`。返回 `cJSON*` 会被自动释放；返回 `std::string` 让 LLM 自然语言播报。

### 音频暂停/恢复（关键！）

任何直接使用 codec 的操作（录音、播放、自检音频）**必须**遵循：
1. 操作前：`audio.Stop()` → `vTaskDelay(100ms)` → `codec->EnableOutput(true)/EnableInput(false)`
2. 操作后：调用 `ResumeAudioService()`——否则设备变聋（唤醒词不会被重新激活）

`ResumeAudioService()` 定义在板文件中，按当前设备状态恢复唤醒词/语音处理。

### 显示/通知系统

- `notification_label_`：底部浮动横幅（`LV_ALIGN_BOTTOM_MID, 0, -70`），默认隐藏，瞬态
- `status_label_`：顶部状态栏，显示"待机"/时钟/状态
- 两者**互斥**：`ShowNotification()` 隐藏 status_label_，`SetStatus()` 隐藏 notification_label_
- 空闲 >10s 时钟抢占已修复（v2.2.0），通知可完整显示满 duration_ms
- 所有 LVGL 访问通过 `DisplayLockGuard` → esp_lvgl_port 全局递归互斥锁串行化（双核 SMP）

### SD 卡文件布局

| 路径 | 内容 |
|---|---|
| `/sdcard/records/` | 录音文件 `rec_YYYYMMDD_HHMMSS.wav`（24kHz/2ch/16bit） |
| `/sdcard/music/` | MP3/AAC/M4A 音乐（支持中文文件名） |
| `/sdcard/logs/` | 系统 SD 日志 `log_YYYYMMDD.txt`（ESP_LOG tee） |
| `/sdcard/logs/chatlogs/` | 对话日志 `chat_<stamp>_<topic>.txt(.wav)`（JSONL + 24kHz/2ch/16bit WAV） |

> ChatLog WAV 与录音 WAV **格式完全一致**，播放路径可复用（仅 downmix 策略不同：录音取 ch0，chatlog 可选 mixed/mic/ai）。

### 串口命令

在 `ScreenshotCmdTask`（`fgets(stdin)` 循环）中处理：`SHOOT`、`LIST`、`LOG`、`SELFTEST`、`CHATLOG`（造测试数据）、`CHATLOGLIST`、`MUSICLIST`、`MUSICPLAY <名>`、`MUSICSTOP`。

---

## 板文件结构

`waveshare-s3-rlcd-4.2.cc` 是**单文件巨模块**（~1900 行），包含：I2C/RTC/SHTC3 驱动、SD 卡管理、录音/音乐/chatlog 管理、MCP 工具注册、自检、截图、串口命令。改动通常都在这一个文件里。

---

## 常见陷阱

1. **COM4 被占用** → 烧录失败"port is busy"：终止残留 monitor 进程后重试
2. **PowerShell commit message** → 圆括号/反引号被解析：用 `git commit -F <文件>`
3. **`ReadShtc3` 未使用警告**（lcd_display.cc:1004）→ 预先存在，非新增，不需修复
4. **第三方警告**（esp_video/ioctl.h `_IO` redefined）→ 预先存在，忽略
5. **音乐播放后设备变聋** → 必须调 `ResumeAudioService()`
6. **中文文件名乱码** → 确认 FATFS 三件套配置齐全
7. **版本号不生效** → 只改 `CMakeLists.txt:7` 的 `PROJECT_VER`，不是 sdkconfig
