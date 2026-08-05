# Waveshare ESP32-S3-RLCD-4.2 小智 AI 设备使用手册

> 适用固件：`waveshare-s3-rlcd-4.2`（小智 XiaoZhi AI v2.1.0 定制分支）
> 板级说明与变更记录见 [main/boards/waveshare-s3-rlcd-4.2/README.md](../main/boards/waveshare-s3-rlcd-4.2/README.md)

---

## 目录

1. [快速开始](#1-快速开始)
2. [按键操作](#2-按键操作)
3. [串口命令](#3-串口命令)
4. [MCP 工具（AI 语音控制）](#4-mcp-工具ai-语音控制)
5. [设备自检](#5-设备自检)
6. [SD 卡录音管理](#6-sd-卡录音管理)
7. [音乐播放（MP3）](#7-音乐播放mp3)
8. [对话日志（聊天记录）](#8-对话日志聊天记录)
9. [屏幕截图](#9-屏幕截图)
10. [配网与网络](#10-配网与网络)
11. [常见问题排查](#11-常见问题排查)

---

## 1. 快速开始

### 1.1 烧录固件

需要 ESP-IDF 5.4+ 环境（版本/路径/串口因机器而异，先自动检测，见 AGENTS.md「构建环境」章节）。

```bash
# 检测当前机器环境（IDF 路径 / Python env / 串口）
python scripts/detect_env.py --export-ps1   # PowerShell 下可用；或直接看摘要

# 激活 + 构建 + 烧录（端口用检测结果，示例为 Windows PowerShell）
$env_out = python scripts/detect_env.py --export-ps1
Invoke-Expression ($env_out -join "`n")
& "$env:IDF_PATH\export.ps1"
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p $env:XIAOZHI_PORT flash
```

### 1.2 串口连接

- 通过 USB-C 连接电脑，设备枚举为 `USB JTAG/serial debug unit`（串口号因机器而异，用 `python scripts/detect_env.py` 检测）
- **主 console 已配置为 USB-Serial/JTAG**：日志输出与串口命令输入都走 USB-C 口，无需额外 USB 转串口模块
- 波特率 `115200`

### 1.3 连接服务器

首次使用：开机后设备进入配网状态 → 按下 BOOT 键 → 手机连接设备热点 → 配置 WiFi → 设备连接小智服务器并激活。

---

## 2. 按键操作

设备有两个按键：**BOOT**（GPIO0）与 **KEY**（GPIO18）。

### 2.1 BOOT 键

| 手势 | 功能 |
|---|---|
| 单击 | 切换聊天状态（开始/停止与 AI 对话） |
| 双击 | 录音开关：开始/停止录音到 SD 卡 |
| 长按 | 截图（P4 PBM 格式，base64 输出到串口） |

### 2.2 KEY 键

| 手势 | 功能 |
|---|---|
| 单击 | 麦克风静音切换（屏上显示 Mic Muted / Mic On） |
| 双击 | 播放提示音 |
| 长按 | 显示系统信息（IP / MAC / 固件版本，通知 6s） |
| 三击 | 播放 SD 卡上最近一条录音 |

> 提示：KEY 键的手势判定窗口较宽（500ms），连按时节奏可以稍慢，仍能被正确识别为三击。

---

## 3. 串口命令

向串口发送以下命令（每行一条，回车结束）：

| 命令 | 功能 | 输出格式 |
|---|---|---|
| `SELFTEST` | 运行硬件自检 | `SELFTEST <项目> PASS/FAIL - <详情>` |
| `SHOOT` | 截屏 | `SCREENSHOT_START` / base64 / `SCREENSHOT_END` |
| `LIST` | 列出 SD 卡录音 | `RECORDINGS_START` / 文件清单 / `RECORDINGS_END` |
| `LOG` | 导出 SD 卡日志文件 | `SDLOG_START <路径>` / 内容 / `SDLOG_END` |
| `MUSICLIST` | 列出 SD 卡音乐 | `MUSIC_START` / 文件清单 / `MUSIC_END` |
| `MUSICPLAY <文件名>` | 播放指定 MP3 | `MUSIC: playing <文件名>` |
| `MUSICSTOP` | 停止音乐播放 | — |
| `CHATLOG` | 触发模拟对话日志（验证 chatlogs 落盘） | `ChatLog: ...` 日志 |
| `CHATLOGLIST` | 列出 SD 卡对话日志（`/sdcard/logs/chatlogs/`） | `CHATLOGS_START ... CHATLOGS_END (N files)` |
| `SYSLOGLIST` | 列出 SD 卡系统日志（`/sdcard/logs/log_*.txt`） | `SYSLOGS_START ... SYSLOGS_END (N files)` |
| `HTTPSTART` | 启动 WiFi HTTP 文件服务器 | `HTTP: started: http://<IP>:80/` |
| `HTTPSTOP` | 停止 HTTP 文件服务器 | `HTTP: stopped` |
| `BTSCAN` | 扫描并连接 BLE 键盘（先让键盘进入配对模式） | 扫描日志 + 连接结果 |
| `BTSTATUS` | 查询 BLE 键盘连接状态 | `BTSTATUS: CONNECTED` / `BTSTATUS: DISCONNECTED` |

使用 `idf.py monitor` 或任意串口工具（115200）即可交互。

---

## 4. MCP 工具（AI 语音控制）

设备侧注册了以下 MCP 工具，AI 在对话中可自动调用：

| 工具 | 参数 | 功能 |
|---|---|---|
| `self.get_temperature_humidity` | — | 查询温湿度（SHTC3 传感器） |
| `self.get_battery_level` | — | 查询电量百分比与充电状态 |
| `self.record_audio` | `duration_seconds` (1–120) | 录音到 SD 卡（默认 5s） |
| `self.list_recordings` | — | 列出最近 10 条录音（文件名/大小/时长） |
| `self.play_recording` | `filename` | 播放指定录音 |
| `self.delete_recording` | `filename` | 删除指定录音 |
| `self.run_self_test` | — | 触发硬件自检 |
| `self.get_self_test_result` | — | 获取最近一次自检结果（JSON） |
| `self.get_version_info` | — | 获取固件版本、构建信息、功能清单、烧录信息（JSON） |
| `self.list_chatlogs` | `directory`(可选 chatlogs/system_logs) | 列出对话日志（默认）或系统日志（log_YYYYMMDD.txt） |
| `self.get_chatlog_summary` | `filename` | 获取指定对话的文本摘要（最多 50 轮，JSON） |
| `self.play_chatlog_audio` | `filename`, `channel`(可选 mixed/mic/ai) | 播放指定对话的音频（可选声道） |
| `self.delete_chatlog` | `filename` | 删除指定对话（.txt + .wav） |
| `self.start_file_server` | — | 启动 WiFi HTTP 文件服务器（mDNS: xiaozhi.local，支持在线播放/查看/删除） |
| `self.stop_file_server` | — | 停止 HTTP 文件服务器（或 10 分钟自动关闭） |
| `self.set_timer` | `duration_minutes`(1-1440), `message`(可选) | 设定倒计时定时器/闹钟，到时播放提示音 |
| `self.list_music` | — | 列出 SD 卡 `/sdcard/music/` 下的 MP3 音乐 |

> **HTTP 服务器页面功能**：文件浏览/下载、WAV 在线播放、ChatLog 聊天记录查看、文件删除、WiFi 配置热更新（`/wifi`）、OTA 在线固件升级（`/upload`）、传感器历史（`/sensors`）。访问 `http://xiaozhi.local/` 或设备 IP。
| `self.play_music` | `filename` | 播放指定 MP3 音乐 |
| `self.stop_music` | — | 停止当前音乐播放 |
| `self.delete_music` | `filename` | 删除指定 MP3 音乐 |
| `self.disp.network` | — | 重新配网 |
| `self.scan_ble` | — | 扫描并连接 BLE 键盘（两段式：第 1 次扫描记住设备并播报名称，第 2 次连接；先让键盘进入配对模式） |
| `self.get_ble_keyboard_status` | — | 查询蓝牙键盘连接状态（已连接/未连接/扫描中，含键盘名） |
| `self.get_ble_keyboard_shortcuts` | — | 播报蓝牙键盘快捷键说明（Enter/Esc/Space/方向键/R/T/M/V/Tab/数字键 1-9） |
| `self.get_voice_commands` | — | 按分类播报全部可用语音命令（蓝牙键盘/环境/录音/音乐/对话日志/系统管理） |

**对话示例**：
- "现在室温多少度？" → `self.get_temperature_humidity`
- "帮我录一段 10 秒的备忘录" → `self.record_audio(10)`
- "自检一下设备" → `self.run_self_test` + `self.get_self_test_result`
- "播放刚才的录音" → `self.play_recording(...)`
- "放首歌听听" → `self.list_music` → `self.play_music("song1.mp3")`
- "音乐停一下" → `self.stop_music`
- "你的版本是多少？" / "你都有什么功能？" → `self.get_version_info`
- "最近聊了什么？" / "对话历史" → `self.list_chatlogs`
- "系统日志" / "logs 目录的文件" → `self.list_chatlogs(directory="system_logs")`
- "那次对话说了什么？" → `self.get_chatlog_summary("chat_..._天气查询.txt")`
- "听一下那段对话录音" → `self.play_chatlog_audio("chat_..._天气查询.wav")`
- "扫描蓝牙键盘" / "连接蓝牙键盘" → `self.scan_ble`（说两次：先扫描后连接）
- "键盘连上了吗？" / "查询键盘状态" → `self.get_ble_keyboard_status`
- "键盘有哪些快捷键？" → `self.get_ble_keyboard_shortcuts`
- "有哪些语音命令？" / "你能做什么？" → `self.get_voice_commands`（按分类介绍全部语音指令）
- "只听小智的声音" → `self.play_chatlog_audio(..., channel="ai")`
- "删掉那条对话" → `self.delete_chatlog("chat_...txt")`
- "下载日志" / "导出文件" → `self.start_file_server`（返回 URL，浏览器打开即可下载）

---

## 5. 蓝牙键盘（BLE HID）

设备支持通过 BLE 连接无线蓝牙键盘作为输入设备（ESP32-S3 仅支持 BLE，不支持经典蓝牙；老式 BT 2.x/3.x 键盘无法配对）。

### 5.1 配对键盘

1. 让键盘进入**配对模式**（BLE 键盘常见方式：长按 Connect 键 / `Fn+C` / `Fn+1`，指示灯快速闪烁）
2. **串口方式**：发送 `BTSCAN` → 再发一次 `BTSCAN`（两段式：第 1 次扫描记住键盘地址，第 2 次连接）
3. **语音方式**：对设备说"扫描蓝牙键盘" → 播报找到的键盘名称后再问一次"扫描蓝牙键盘"即连接
4. 连接成功后屏幕显示"键盘已连接"（Just Works 配对，无需输入密码）
5. 随时可用 `BTSTATUS` 串口命令查询连接状态（`BTSTATUS: CONNECTED` / `DISCONNECTED`）

> ⚠️ **注意**：配对前务必先让键盘进入配对模式。若键盘未配对，连接会挂起 30 秒超时（期间占用部分内存，超时后自动释放）。

### 5.2 快捷键映射

| 按键 | 功能 |
|---|---|
| Enter | 开始/停止与 AI 对话 |
| Esc | 停止监听 |
| Space | 开始监听 |
| ↑ / ↓ | 音量 ±10 |
| R | 截图 |
| T | 开始/停止录音 |
| M | 麦克风静音切换 |
| V | 版本信息弹窗 |
| Tab | 播放提示音 |
| 1 | 播放提示音 |
| 2 | 重置网络（进入配网模式） |
| 3 | 系统信息弹窗 |
| 4 | 播放最近一条录音 |
| 5 | 电量检查 |
| 6 | HTTP 服务器开关 |
| 7 | 停止 HTTP 服务器 |
| 8 | 触发唤醒词 |
| 9 | 重启设备 |

> 💬 **语音查询快捷键**：问"键盘有哪些快捷键"即可播报上表。

### 5.3 自动重连与断开

- **自动重连**：连接成功过的键盘地址保存在 NVS，设备重启后约 8 秒**自动定向重连**（仅连接已知地址，不扫描，安全无泄漏）；键盘不在线则自动跳过，可随时手动 `BTSCAN`
- 键盘关闭或远离后自动断开，屏幕显示"键盘已断开"
- 重新连接需再次让键盘进入配对模式 + `BTSCAN`

---

## 6. 设备自检

对标产测固件，一键验证 7 项硬件通路。

### 6.1 触发方式

| 方式 | 操作 |
|---|---|
| 串口 | 发送 `SELFTEST` |
| AI 对话 | 让 AI 调用 `self.run_self_test` |

### 6.2 测试项

| 测试项 | 验证内容 |
|---|---|
| Display | 像素 LUT 映射回环 |
| Buttons | BOOT/KEY GPIO 电平（信息项） |
| SDCard | 写/读回探针文件并比对 |
| Battery | 电池 ADC 电量 |
| RTC | PCF85063 时间读取（年份校验） |
| SHTC3 | 温湿度读数（范围校验） |
| Audio | 录音 300ms → 喇叭回放（环回验证麦克风→编解码→喇叭通路） |

> 设备正在对话/录音时，Audio 项自动跳过（`skipped (busy)`），不影响其他项目。

### 6.3 结果查看

- **屏幕**：每项完成时通知显示 `SELFTEST <项目> PASS/FAIL`，最后显示汇总 `SelfTest: 7/7 ALL PASS`
- **串口**：完整逐项日志（`SELFTEST ... PASS - <详情>`）
- **AI**：调用 `self.get_self_test_result` 获取 JSON（含每项 pass/detail、passed/total）

**正常输出示例**：
```
SELFTEST Display  PASS - LUT ok
SELFTEST Buttons  PASS - BOOT=1 KEY=1
SELFTEST SDCard   PASS - RW ok
SELFTEST Battery  PASS - 98% chg=0
SELFTEST RTC      PASS - 2026-08-01 16:42
SELFTEST SHTC3    PASS - 26.2C 39%
SELFTEST Audio    PASS - cap=300ms play=300ms
SELFTEST SelfTest: 7/7 ALL PASS
```

---

## 7. SD 卡录音管理

### 7.1 目录结构

- 录音：`/sdcard/records/rec_YYYYMMDD_HHMMSS.wav`（24kHz 立体声 16bit WAV）
- 日志：`/sdcard/logs/log_YYYYMMDD.txt`（系统日志自动落盘）

### 7.2 录音方法

| 方式 | 操作 |
|---|---|
| BOOT 双击 | 开始/停止手动录音 |
| AI 对话 | `self.record_audio(duration_seconds)` 定时录音（1–120s） |
| 串口 | 无直接命令（用按键或 AI） |

### 7.3 播放方法

| 方式 | 操作 |
|---|---|
| KEY 三击 | 播放最近一条录音 |
| AI 对话 | 先 `self.list_recordings` 再 `self.play_recording(filename)` |

> 播放时设备会自动暂停 AI 对话，播放结束自动恢复。

---

## 8. 音乐播放（MP3）

设备支持播放 SD 卡 `/sdcard/music/` 目录下的 **MP3 音乐文件**（使用官方 `esp_audio_codec` 解码，自动处理立体声→单声道，并支持任意采样率重采样至 24kHz，兼容常见 44.1/48kHz 音乐）。

### 8.1 放入音乐

将 MP3 文件复制到 SD 卡的 `music` 目录：

```
/sdcard/music/
├── song1.mp3
├── song2.mp3
└── ...
```

> 首次挂载 SD 卡时固件会自动创建 `music` 目录。
> **支持中文文件名**（固件 FATFS 已配置 UTF-8 编码 + 简体中文字符集），如 `凄美地.mp3` 可直接播放。

### 8.2 操作方法

| 方式 | 操作 |
|---|---|
| 串口 | `MUSICLIST` 列出歌曲；`MUSICPLAY 歌曲名.mp3` 播放；`MUSICSTOP` 停止 |
| AI 对话 | 让 AI 调用 `self.list_music` → `self.play_music(filename)` → `self.stop_music` |
| 删除歌曲 | AI 调用 `self.delete_music(filename)` |

### 8.3 MCP 工具

| 工具 | 参数 | 功能 |
|---|---|---|
| `self.list_music` | — | 列出 `/sdcard/music/` 下所有 MP3（文件名/大小） |
| `self.play_music` | `filename` | 播放指定 MP3 |
| `self.stop_music` | — | 停止当前播放 |
| `self.delete_music` | `filename` | 删除指定 MP3 |

### 8.4 串口命令

| 命令 | 功能 |
|---|---|
| `MUSICLIST` | 列出音乐文件 |
| `MUSICPLAY <文件名>` | 播放指定音乐（如 `MUSICPLAY song1.mp3`） |
| `MUSICSTOP` | 停止播放 |

### 8.5 播放时屏幕显示

播放音乐时，屏幕底部通知栏会依次显示：

1. **歌曲名**：`>> 歌曲名.mp3`（持续 8s）
2. **音频参数**：采样率 + 声道，如 `44.1kHz Stereo` / `48kHz Mono`（4s）
3. **播放进度**：每 5s 更新一次 `歌曲名 百分比% 已播时长(分:秒)`，如 `凄美地.mp3 6% 00:15`

> 播放进度百分比基于文件字节位置（VBR MP3 时与时间进度略有偏差），已播时长基于解码的音频样本精确计算。

**对话示例**：
- "播放一首歌" → `self.list_music` → `self.play_music("song1.mp3")`
- "停止音乐" → `self.stop_music`
- "删除这首歌" → `self.delete_music("song2.mp3")`

> 播放音乐时设备自动暂停 AI 对话；音乐播放完成或停止后自动恢复 AI 服务。
> 播放期间唤醒词与按键对话暂停（音乐占用麦克风/喇叭通路），结束后自动恢复唤醒词。
> 正在播放时不能再次播放（需先 `self.stop_music`）。

---

## 9. 对话日志（聊天记录）

设备自动把每次 AI 对话的**文本**和**语音**保存到 SD 卡 `/sdcard/logs/chatlogs/` 目录，按**对话时间 + 主题**命名。

### 9.1 保存内容

| 文件 | 格式 | 内容 |
|---|---|---|
| `.txt` | JSONL（每行一条） | 对话文本：时间戳 + 角色(user/assistant) + 内容 |
| `.wav` | 24kHz 立体声 16bit | 音频：ch0=用户麦克风，ch1=AI 喇叭（AEC 参考回采） |

### 9.2 文件名规则

```
/sdcard/logs/chatlogs/
├── chat_20260801_201417_设备调试.txt   ← 会话名 = 起始时间 + 主题
└── chat_20260801_201417_设备调试.wav   ← 同名音频
```

- **时间**：会话开始的 `YYYYMMDD_HHMMSS`（RTC/NTP 时间）
- **主题**：自动取**首条用户语音识别文本**（截断 20 字），如"今天天气怎么样"

### 9.3 对话文本示例（.txt）

```json
{"ts":"2026-08-01 20:14:17","role":"user","text":"今天天气怎么样"}
{"ts":"2026-08-01 20:14:18","role":"assistant","text":"今天天气晴朗，气温26度。"}
```

### 9.4 说明

- 会话从**音频通道打开**（唤醒/按键开始对话）到**通道关闭**结束
- 音频通过 AEC 参考通道同步捕获 AI 说话声，无需额外混音
- 文本按 1s 节流 fsync 落盘，音频在会话结束时写入 WAV 头
- **无需 SD 卡时自动跳过**（不影响对话）

### 9.5 调试命令

串口发送 `CHATLOG` 可触发一次**模拟会话**（写入测试文本+音频），用于验证日志功能：

```
I (10482) ChatLog: Conversation log started: /sdcard/logs/chatlogs/chat_20260801_201417.txt
I (10492) ChatLog: chat user: 今天天气怎么样
I (10562) ChatLog: chat assistant: 今天天气晴朗，气温26度。
I (10662) ChatLog: Conversation log ended: chat_20260801_201417_设备调试.txt (21888 bytes audio)
```

---

## 10. 屏幕截图

- **BOOT 长按**：截取当前屏幕，输出 P4 PBM 格式（base64，72 字符/行）
- **串口 `SHOOT`**：同上
- **开机自动截图**：设备启动约 12s 后自动截图并导出 SD 日志（便于无头调试）

配套 Python 工具见仓库 `tools/screenshot.py`（如存在）。

---

## 11. 配网与网络

- **配网模式**：开机时按住 BOOT 或串口发送 `self.disp.network` 触发
- **网络事件**：连接状态通过屏幕状态栏图标显示
- **NTP 时间**：连接服务器后自动同步，并回写 PCF85063 RTC（离线时也能保持时间）

---

## 12. 常见问题排查

| 现象 | 可能原因 / 处理 |
|---|---|
| 串口命令无响应 | 确认主 console 为 USB-Serial/JTAG（本固件已默认）；确认波特率 115200 |
| 自检显示 `Audio skipped (busy)` | 设备正在对话/录音，稍后重试 |
| 自检 `SDCard FAIL` | SD 卡未插入或未挂载，检查卡接触 |
| 自检 `Battery FAIL` | ADC 初始化失败（固件已降级处理，不影响其他功能），重启重试 |
| 自检 `RTC FAIL` | PCF85063 通信异常或电池耗尽导致时间无效，连接服务器同步后重试 |
| WiFi 无法连接 | 重新配网：`self.disp.network` 或开机长按 BOOT |
| 屏幕无显示 | 反射式 LCD 无背光，需环境光；确认 SPI 排线连接 |
| `MUSICLIST` 显示 0 文件 | 确认 MP3 文件放在 SD 卡 `/sdcard/music/` 目录 |
| `MUSICPLAY` 提示文件不存在 | 文件名需与 `MUSICLIST` 输出完全一致（含扩展名 .mp3） |
| 音乐播放声音异常/变速 | MP3 采样率非标准（如 32kHz）时自动重采样，极端码率偶发变速可换标准 44.1kHz 文件 |
| `self.play_music` 返回"already playing" | 当前有音乐在播，先调用 `self.stop_music` |
| `chatlogs` 目录无新对话文件 | 对话需连接服务器（音频通道打开才记录）；SD 卡未插入时不记录 |
| 对话 `.txt` 中文乱码 | 文件为 UTF-8 编码，用支持 UTF-8 的编辑器（VS Code/记事本）打开 |
| `BTSCAN` 扫描不到键盘 | 确认键盘已进入配对模式（指示灯快闪）；ESP32-S3 仅支持 BLE 键盘（BT 2.x/3.x 不支持） |
| 键盘连接后按键无反应 | 确认键盘在 Report 模式；部分键盘需先配对授权（重新 `BTSCAN`） |
| 键盘连接占用内存后功能异常 | 连接失败会 30s 超时自动释放；避免在未配对状态下反复 `BTSCAN` |

---

*最后更新：2026-08-01*
