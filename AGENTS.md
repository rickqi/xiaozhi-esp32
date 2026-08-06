# AGENTS.md

本文件为 OpenCode 会话提供仓库专属指引，只记录"不看会踩坑"的高密度信息。

---

## 项目概述

小智 AI 聊天机器人 ESP32 固件（78/xiaozhi-esp32 定制分支），目标板 **waveshare-s3-rlcd-4.2**（ESP32-S3，4.2 寸圆形 ST7305 单色 LCD，ES8311+ES7210 音频，SHTC3 温湿度，PCF85063 RTC，SD 卡）。

---

## 构建环境（多环境自动检测）

> **本仓库不硬编码单机环境路径**（环境迁移/换机后路径、串口号、Python 版本都会变）。
> 所有构建/烧录/串口操作前，**先运行 `scripts/detect_env.py` 检测当前机器的真实环境**。

### 环境检测（每个会话/每台新机器必做）

```powershell
python scripts/detect_env.py              # 人类可读摘要：IDF 路径/版本、Python env、串口
python scripts/detect_env.py --json       # JSON 输出（供脚本/agent 消费）
python scripts/detect_env.py --check      # 与 scripts/env_expect.json 快照对比，判定环境冲突（exit 2=有差异）
python scripts/detect_env.py --health     # 健康检查：验证是否满足开发/调试/烧录要求（exit 2=不满足）
python scripts/detect_env.py --export-ps1 # 生成 PowerShell 变量赋值，可直接执行/落盘
```

**检测优先级**（脚本内实现）：环境变量 `IDF_PATH` > 官方安装器元数据 `~/.espressif/idf-env.json` > 常见安装路径探测。
**串口识别**：优先匹配 Espressif USB-Serial/JTAG（VID `0x303A`，即本板调试口），其次 USB-UART 桥接芯片。

**`--health` 检查项（10 项）**：IDF 路径/版本、Python venv 存在性、venv 版本标记匹配（`idf_version.txt`）、Python 依赖满足 IDF 约束（`idf_tools.py check-python-dependencies`）、工具链（`idf_tools.py check`）、串口可打开、**sdkconfig 芯片/板/console/FATFS/唤醒词配置**、分区表、构建产物。任一 FAIL 即 exit 2。

> **sdkconfig 漂移陷阱**：环境迁移后 `sdkconfig` 可能是别的目标/板的旧配置（例如 esp32 + bread-compact-esp32，而本板是 esp32s3 + waveshare-s3-rlcd-4.2）。此时 `idf.py build` 会"成功"但编译的是错误板！务必先 `python scripts/detect_env.py --health` 确认 `sdkconfig 芯片/板` 项 PASS；否则执行 `idf.py set-target esp32s3` 重新生成。

### 标准工作流（激活 + 构建 + 烧录）

```powershell
# 1. 检测并导出环境变量（每台机器自动识别，无需手动改端口/路径）
$env_out = python scripts/detect_env.py --export-ps1
Invoke-Expression ($env_out -join "`n")  # 设置 $env:IDF_PATH / $env:XIAOZHI_PORT 等

# 2. 激活 ESP-IDF（路径来自检测结果）
& "$env:IDF_PATH\export.ps1"

# 3. 构建 + 烧录 + 监控（端口来自检测结果）
idf.py build
idf.py -p $env:XIAOZHI_PORT flash
idf.py -p $env:XIAOZHI_PORT monitor
```

> **注意**：`idf.py monitor` 会独占串口，烧录前必须先终止 monitor 进程。

### 串口命令测试（不阻塞，适合自动化验证）

```powershell
# 用检测到的 Python 解释器（保证含 pyserial）+ 检测到的串口
& "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" -c "
import serial, time
s = serial.Serial('$env:XIAOZHI_PORT', 115200, timeout=0.3)
time.sleep(0.3); s.read(4096)  # drain
s.write(b'CHATLOGLIST\n'); s.flush(); time.sleep(3); print(s.read(4096).decode('utf-8','replace'))
"
```

### 多环境迁移 / 冲突判定

| 场景 | 处理方式 |
|---|---|
| 换电脑 / IDF 重装 / 串口变化 | 重跑 `detect_env.py --check`，exit 2 表示与快照不一致 |
| 快照过时 | 以 `detect_env.py` 检测结果为准，确认后更新 `scripts/env_expect.json`（`_更新时间` 一并改） |
| 多台机器共用仓库 | 每台机器各自运行检测即可，`env_expect.json` 只记录"上次验证"的机器 |
| 检测不到串口 | 手动指定：`idf.py -p COMx flash`；检查设备是否枚举为 USB-Serial/JTAG |

> 版本号/固件相关信息（`PROJECT_VER`、`GIT_COMMIT`）由构建系统注入，与机器无关，不受环境迁移影响。

---

## Git 与分支

| 分支 | 说明 |
|---|---|
| `master` | **当前工作分支**，定制功能线（当前 v3.7.0），已推送 origin |
| `rebuild-v2.4` | 官方 v2.4.0 基线 + 移植功能，本地实验分支，未推送 |
| `origin/main` / `origin/master` | 远程（origin = `https://github.com/rickqi/xiaozhi-esp32.git`） |

> master 与 rebuild-v2.4 **无共同祖先**，不可直接 merge/cherry-pick。
> 版本演进：v3.2.x 蓝牙键盘 MVP → v3.4.x 连接稳定性修复（SD 日志/LCD SPI/配对）→ v3.5.x 语音扫描/自动重连/构建信息更新 → v3.6.x 帮助类语音工具 → v3.7.0 字体系统（fallback 链/图标覆盖）。

**提交规范**：`feat:` / `fix:` 前缀 + 中文或英文描述。多行 message 用 `git commit -F <file>` 避免 PowerShell 解析问题（圆括号、反引号会被 shell 误解析）。

---

## 版本管理

**版本号唯一来源**：`CMakeLists.txt:7` 的 `set(PROJECT_VER "...")`，自动注入 `esp_app_desc_t.version`，传播到 OTA / 显示 / MCP / User-Agent。

**Git 提交戳**：`main/CMakeLists.txt` 在构建时执行 `git rev-parse --short HEAD`，通过 `GIT_COMMIT` 宏嵌入固件（失败回退 `"unknown"`）。
- v3.5.1 起：`CMAKE_CONFIGURE_DEPENDS .git/HEAD`（提交后自动重新 configure）+ `build_stamp` 目标每次构建强制重编 `version_info.cc`（git 提交 + 编译时间始终新鲜）。
- **⚠️ 烧录前先提交**：若在提交前烧录，固件嵌入的是**旧 HEAD 的 GIT_COMMIT**（v3.5.0 曾嵌入提交前的 624e293，版本信息显示"版本新但提交旧"）。烧录前先 `git commit` 再 `idf.py flash`。

### 版本 bump 判断准则

| 改动类型 | 版本 bump | 示例 |
|---|---|---|
| 纯 bug 修复、显示修正 | patch（x.y.**Z**） | C1 通知空闲时钟抢占修复 |
| 新增功能、MCP 工具、新能力 | minor（x.**Y**.0） | chatlog 管理、版本信息工具 |
| 分区变更、协议破坏、不兼容改动 | major（**X**.0.0） | OTA 协议改版、分区表不兼容 |

> 每次提交前判断是否需要 bump 版本；有新功能必须 bump minor。

### 版本更新强制要求

**每次版本 bump 必须同步更新以下内容**（缺一不可）：

1. **`CHANGELOG.md`**：在该文件顶部（最新版本）增加本次变更条目，按 `feat` / `fix` / `change` 分类，简述改了什么。**必须包含本次版本的具体变更说明，不能只写"本次"或笼统描述。**
2. **`main/version_info.cc` 的 `kFeatures[]`**：如果新增/删除/变更了功能，同步更新功能清单数组（语音查询 `self.get_version_info` 时播报的就是这个数组）
3. **版本号**：`CMakeLists.txt:7` 的 `set(PROJECT_VER "...")`
4. **文档**：`docs/usage.md` 和板 `README.md` 中受影响的 MCP 工具表 / 串口命令表 / 功能说明

> 纯文档提交（如 AGENTS.md、分析报告）**不需要 bump 版本**，也无需更新 CHANGELOG。
> v3.0.0 起，分区表调整属于 major bump，需 USB 烧录（`idf.py erase-otadata && idf.py flash`）。

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

使用 **v2 分区**（`partitions/v2/16m.csv`，v3.0.0 调整后）：双 OTA 槽 `ota_0`/`ota_1`（各 ~4.63MB）+ `assets`（SPIFFS，6.63MB，唤醒词/字体/表情/音频）。

> v1 与 v2 **不兼容**，无法互相 OTA。v3.0.0 调整了 OTA/assets 大小，v2.x 无法 OTA 到 v3.0.0，需 USB 烧录。
> `main/CMakeLists.txt` 末尾会自动检测 `assets` 分区并注册到 `idf.py flash`。

---

## 代码架构要点

### MCP 工具注册（板级）

所有设备端 MCP 工具在 `main/boards/waveshare-s3-rlcd-4.2/waveshare-s3-rlcd-4.2.cc` 的 `InitializeTools()` 中注册。

| 方法 | AI 可见 | 语音可调用 |
|---|---|---|
| `mcp_server.AddTool("self.xxx", ...)` | ✅ | ✅ |
| `mcp_server.AddUserOnlyTool("self.xxx", ...)` | ❌ | ❌ |

> `self.get_system_info` 是 user-only（AI 不可见）；如需语音查询版本/系统信息，用 `AddTool` 注册的 `self.get_version_info`。

**语音可调用工具全家桶（AddTool，板级 ~25 + 框架级 3）**：
- 蓝牙键盘：`self.scan_ble`（两段式扫描+连接）、`self.get_ble_keyboard_status`（连接状态）、`self.get_ble_keyboard_shortcuts`（快捷键）
- 帮助类：`self.get_voice_commands`（按分类介绍全部语音命令）、`self.get_mcp_help`（带可选 `tool_name` 参数——不填返回全部工具概览，填工具名返回该工具详细用法）
- 其他：环境/录音/音乐/对话日志/自检/文件服务器/定时器（见 `docs/usage.md` MCP 表）

ReturnValue 类型：`std::variant<bool, int, std::string, cJSON*, ImageContent*>`。返回 `cJSON*` 会被自动释放；返回 `std::string` 让 LLM 自然语言播报。

### MCP 工具回调必须非阻塞（关键！）

**所有 `AddTool` 回调运行在 `app_main` 主任务**（`McpServer::DoToolCall` → `app.Schedule()` → `Application::Run()` 单线程事件循环）。工具结果经 `SendMcpMessage` **再次 Schedule** 发送。

- **阻塞 >5s → 语音命令必报"超时"**：服务器端 MCP 工具调用超时窗口 ~10s，设备 13s 阻塞必然超时（v3.5.0 教训：`self.scan_ble` 曾 `WaitScanComplete(13000)` 阻塞主任务 → 语音全超时，v3.5.1 改异步）。
- 耗时操作（扫描/连接/录音）→ 启动异步任务（`xTaskCreate`）立即返回，结果供后续查询；回调内**绝不做同步等待**。
- 阻塞主任务还会饿死音频上传/UI/状态机。

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

在 `ScreenshotCmdTask`（`fgets(stdin)` 循环）中处理：`SHOOT`、`LIST`、`LOG`、`SELFTEST`、`CHATLOG`（造测试数据）、`CHATLOGLIST`、`MUSICLIST`、`MUSICPLAY <名>`、`MUSICSTOP`、`BTSCAN`（扫描/连接 BLE 键盘，需发两次：第 1 次扫描记住地址，第 2 次连接）、`BTSTATUS`（查询键盘连接状态，输出 `CONNECTED`/`DISCONNECTED`）。

### 蓝牙键盘（BLE HID）

- **两段式连接**：`BTSCAN` 或语音 `self.scan_ble` 需调用两次——第 1 次扫描保存 pending 地址，第 2 次触发连接（防自动连接不可达键盘的内存泄漏，v3.2.1 教训）。
- **RPA 地址轮换**：MIIIW 键盘每次重广播地址都变（`df:3b:27:5e:XX:d5` 末字节轮换）——自动重连可能因地址失效而失败，静默等待手动 BTSCAN 即可。
- **自动重连（v3.5.0）**：连接成功键盘地址持久化 NVS（namespace `kbd`）；开机 8s 后**仅定向连接**该地址（绝不扫描 → 零泄漏）；NVS 中全零地址视为无效跳过。
- **快捷键**：Enter=对话 / Esc=停止 / Space=监听 / ↑↓=音量 / R=截图 / T=录音 / M=静音 / V=版本 / Tab=提示音 / 数字键 1-9（在 `HandleKeyboardKey()`，约 L381）。

### 字体系统（v3.7.0）

- **双层**：内置 `font_puhui_basic_30_4`（flash）+ assets 全量 `font_puhui_common_30_4.bin`（mmap 零拷贝，18000+ 字）；版本弹窗另用 `font_puhui_basic_14_1`。
- **LVGL 9.3.0 软件渲染无字形缓存**（v8 缓存已移除）——fallback 链零额外 RAM。
- assets 分区是**自定义 mmap blob**（非真 SPIFFS）：44 字节/文件条目 + `0x5A5A` 魔数 + 16 位校验和，格式见 `docs/font-system-design.md`。
- **fallback 设置**：flash 中 const 字体不可写；用堆分配的 cbin 字体（`cbin_font_create()`）直接赋 `font->fallback`（v3.7.0 已实现于 `assets.cc` Apply 内）。

---

## 板文件结构

`waveshare-s3-rlcd-4.2.cc` 是**单文件巨模块**（~3100 行），包含：I2C/RTC/SHTC3 驱动、SD 卡管理、录音/音乐/chatlog 管理、MCP 工具注册、自检、截图、串口命令、蓝牙键盘回调。改动通常都在这一个文件里。

---

## 常见陷阱

1. **串口被占用** → 烧录失败"port is busy"/`PermissionError: 拒绝访问`：检查残留进程（`Get-CimInstance Win32_Process | Where-Object {$_.CommandLine -match 'COM3|idf.py|esptool|ninja'}`），终止 `idf.py`/`ninja`/`esptool`/`ldgen.py` 僵尸进程后重试；有时等待几秒 USB 重新枚举即可
2. **PowerShell commit message** → 圆括号/反引号被解析：用 `git commit -F <文件>`
3. **`ReadShtc3` 未使用警告**（lcd_display.cc:1004）→ 预先存在，非新增，不需修复
4. **第三方警告**（esp_video/ioctl.h `_IO` redefined）→ 预先存在，忽略
5. **音乐播放后设备变聋** → 必须调 `ResumeAudioService()`
6. **中文文件名乱码** → 确认 FATFS 三件套配置齐全
7. **版本号不生效** → 只改 `CMakeLists.txt:7` 的 `PROJECT_VER`，不是 sdkconfig
8. **MCP 工具回调阻塞 >5s** → 语音必报"超时"（服务器工具超时 ~10s）：耗时操作转异步任务，回调立即返回
9. **构建失败 `UnboundLocalError: pre_loc`（pyparsing）** → IDF 5.5.2 需 `pyparsing>=3.1.0,<3.3`；3.2.0 有 bug 导致 `ldgen.py` 崩。`pip install pyparsing==3.2.1` 后重构建（勿装 3.3.x，不满足 IDF 约束）
10. **版本信息"版本新但提交旧"** → 烧录前先 `git commit`（GIT_COMMIT 构建时注入旧 HEAD，见版本管理节）
11. **LCD SPI 瞬时失败**（`panel_io_spi_tx_color`）→ 已容错（v3.4.7）：RLCD 发送失败重试不 abort，扫描期间 NimBLE 抢占可能触发，属正常 WARN
12. **`idf.py build` 偶发挂起** → 超时后无进程残留再重跑；或改用 `cmd /c "call %IDF_PATH%\export.bat >nul 2>&1 && idf.py build"`（export.ps1 偶发异常）
