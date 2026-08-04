# Waveshare ESP32-S3-RLCD-4.2 小智 AI 板 — 功能说明与变更记录

> 本 README 记录该板小智固件的定制功能、操作方法及变更历史。
> 基础固件：XiaoZhi AI v2.1.0（78/xiaozhi-esp32），板级目录：`main/boards/waveshare-s3-rlcd-4.2/`

📖 **完整使用手册（快速开始 / 按键 / 串口 / MCP / 自检 / 排查）见 [docs/usage.md](../../../docs/usage.md)**

---

## 硬件特性

- **主控**：ESP32-S3（PSRAM 8MB / Flash 16MB）
- **显示屏**：4.2" 圆形反射式 LCD（ST7305，400×300，1bit 单色，无背光）
- **音频**：ES8311 喇叭输出 + ES7210 双麦克风输入（24kHz 立体声，含 AEC 参考音）
- **传感器**：SHTC3 温湿度（I2C 0x70）、PCF85063 RTC（I2C 0x51）、电池 ADC（ADC1_CH3）
- **存储**：SD 卡（SDMMC 1-bit：CLK38 / CMD21 / D0 39）
- **按键**：BOOT（GPIO0）、KEY（GPIO18）
- **通信**：WiFi（USB-Serial/JTAG 作串口 console）

---

## 新增功能（本分支定制）

### 1. 设备自检模式（Self-Test）⭐ 核心新增

对标产测固件，一键逐项验证硬件通路，结果上屏 + 串口 + MCP 返回。

**测试项（7 项）**：

| 测试项 | 验证内容 | 判定标准 |
|---|---|---|
| Display | 像素 LUT 回环（写角落像素→读回） | 写入与读回一致 |
| Buttons | BOOT/KEY GPIO 电平 | 信息项，恒 PASS |
| SDCard | 写/读回探针文件并比对 | 字节数一致 + 内容一致 |
| Battery | 电池 ADC 电量 + 充电状态 | ADC 读取成功 |
| RTC | 读 PCF85063 时间 | 年份在 2000–2099 |
| SHTC3 | 温湿度读数 | 温度 -40~125°C、湿度 0~100% |
| Audio | 录音 300ms → 喇叭回放（环回） | 采集与回放均完成；设备忙时自动跳过 |

**触发方式**：
- **串口命令**：向串口发送 `SELFTEST`（无需连接 AI 服务器）
- **MCP 工具**：AI 侧调用 `self.run_self_test`；查询上次结果用 `self.get_self_test_result`

**输出示例**：
```
I (21836) waveshare_rlcd_4_2: SELFTEST Display  PASS - LUT ok
I (21836) waveshare_rlcd_4_2: SELFTEST Buttons  PASS - BOOT=1 KEY=1
I (21836) waveshare_rlcd_4_2: SELFTEST SDCard   PASS - RW ok
I (21836) waveshare_rlcd_4_2: SELFTEST Battery  PASS - 98% chg=0
I (21836) waveshare_rlcd_4_2: SELFTEST RTC      PASS - 2026-08-01 16:42
I (21836) waveshare_rlcd_4_2: SELFTEST SHTC3    PASS - 26.2C 39%
I (21836) waveshare_rlcd_4_2: SELFTEST Audio    PASS - cap=300ms play=300ms
I (21836) waveshare_rlcd_4_2: SELFTEST SelfTest: 7/7 ALL PASS
```

### 2. MCP 工具（设备端，AI 可调用）

| 工具 | 功能 |
|---|---|
| `self.disp.network` | 重新配网 |
| `self.get_temperature_humidity` | 查询温湿度（SHTC3） |
| `self.get_battery_level` | 查询电量/充电状态 |
| `self.record_audio` | 录音到 SD 卡（1–120s） |
| `self.list_recordings` | 列出 SD 卡录音 |
| `self.play_recording` | 播放指定录音 |
| `self.delete_recording` | 删除录音 |
| `self.run_self_test` | 触发硬件自检 |
| `self.get_self_test_result` | 获取上次自检 JSON 结果 |
| `self.get_version_info` | 查询固件版本 / 构建信息 / 功能清单 / 烧录信息（语音可调用） |
| `self.list_chatlogs` | 列出对话日志（默认）或系统日志（directory 参数可选 chatlogs/system_logs） |
| `self.get_chatlog_summary` | 获取指定对话的文本摘要（最多 50 轮） |
| `self.play_chatlog_audio` | 播放指定对话音频（可选声道 mixed/mic/ai） |
| `self.delete_chatlog` | 删除指定对话（.txt + .wav） |
| `self.start_file_server` | 启动 WiFi HTTP 文件服务器（浏览器下载 SD 卡文件） |
| `self.stop_file_server` | 停止 HTTP 文件服务器 |
| `self.list_music` | 列出 SD 卡 `/sdcard/music/` 下 MP3 音乐 |
| `self.play_music` | 播放指定 MP3（自动解码+重采样） |
| `self.stop_music` | 停止音乐播放 |
| `self.delete_music` | 删除指定 MP3 |

### 3. 按键功能

| 按键 | 手势 | 功能 |
|---|---|---|
| BOOT | 单击 | 切换聊天状态 / 配网中进入配网模式 |
| BOOT | 双击 | 切换录音到 SD 卡 |
| BOOT | 三击 | 全屏显示固件版本信息+变更摘要+按键说明（14px字体，15秒） |
| BOOT | 长按 | 截屏（P4 PBM → base64 输出到串口） |
| KEY | 单击 | 麦克风静音切换 |
| KEY | 双击 | 播放提示音 |
| KEY | 长按 | 显示系统信息（IP / MAC / 版本） |
| KEY | 三击 | 播放最近一条录音 |

### 4. 串口调试命令

| 命令 | 功能 |
|---|---|
| `SHOOT` | 截屏（SCREENSHOT_START/END 协议输出） |
| `LIST` | 列出 SD 卡录音（文件名/大小/时长） |
| `LOG` | 导出 SD 卡日志文件 |
| `SELFTEST` | 运行硬件自检 |
| `MUSICLIST` | 列出 SD 卡音乐（/sdcard/music/） |
| `MUSICPLAY <文件名>` | 播放指定 MP3（如 `MUSICPLAY song1.mp3`） |
| `MUSICSTOP` | 停止音乐播放 |
| `CHATLOG` | 触发模拟对话日志（验证 chatlogs 落盘） |
| `CHATLOGLIST` | 列出 SD 卡对话日志（chatlogs 目录） |
| `SYSLOGLIST` | 列出 SD 卡系统日志（logs 目录 log_*.txt） |
| `HTTPSTART` | 启动 WiFi HTTP 文件服务器 |
| `HTTPSTOP` | 停止 HTTP 文件服务器 |
| `BTSCAN` | 扫描并连接 BLE 键盘（先让键盘进入配对模式） |

### 5. 音乐播放（MP3）

- 播放 SD 卡 `/sdcard/music/` 目录下的 MP3 文件（固件自动创建该目录，**支持中文文件名**）
- 解码使用官方 **`espressif/esp_audio_codec` v2.6.1**（ESP_AUDIO_SIMPLE_DEC_TYPE_MP3）
- 处理链路：`MP3 解码 → 立体声→单声道 → 线性插值重采样(任意采样率→24k) → codec->OutputData()`
- FATFS 配置为 **UTF-8 文件名 + codepage 936**（`sdkconfig.defaults.esp32s3`），中文名歌曲可正常识别/播放
- 播放时自动暂停 AI 对话，结束/停止后自动恢复；支持 `MUSICSTOP` 中途停止
- 详见 [docs/usage.md 第 7 章](../../../docs/usage.md)

### 6. 对话日志（ChatLog）

- 自动将每次 AI 对话的**文本 + 语音**保存到 `/sdcard/logs/chatlogs/`
- 文件名 = **对话起始时间 + 首句主题**：`chat_20260801_201417_设备调试.txt/.wav`
- `.txt` 为 JSONL（时间戳 + 角色 + 内容）；`.wav` 为 24kHz 立体声（ch0=麦克风，ch1=AI 喇叭 AEC 回采）
- 文本 hook：`Application::OnIncomingJson` 的 stt/tts 分支（application.cc）
- 音频 tap：`AudioService` 新增 `on_input_raw`/`on_output_pcm` 回调（AEC 参考通道天然含 AI 声音）
- 会话边界：音频通道打开/关闭（`OnAudioChannelOpened/Closed`）
- 串口 `CHATLOG` 命令可触发模拟会话验证
- 详见 [docs/usage.md 第 8 章](../../../docs/usage.md)

---

## 代码优化（本次变更）

| 优化点 | 说明 |
|---|---|
| **I2C 设备句柄缓存** | RTC/SHTC3 访问不再每次 `add_device/rm_device`，改为懒加载缓存句柄（`GetI2cDevice()`） |
| **SD 日志 fsync 节流** | 每行 `fsync` 落卡改为 `fflush` 每行 + `fsync` 最多 1s 一次，避免日志路径阻塞所有 `ESP_LOG*` 调用 |
| **ADC 初始化不致命** | ADC 初始化失败不再 `ESP_ERROR_CHECK` 死机，降级为电量不可用 |
| **调试任务门控** | GPIO18 电平监控任务（10ms 轮询）默认关闭，需 `#define RLCD_ENABLE_KEY_LEVEL_MONITOR 1` 启用 |
| **清理** | 移除未用变量 `vbat_status`、重复 include、失效变量引用 |
| **Console 配置** | 主 console 由 UART0 切换为 **USB-Serial/JTAG**（该板无独立 UART 桥接），串口命令与日志均走 USB-C 口 |

---

## 构建与烧录

```bash
# 需 ESP-IDF 5.4+，选择目标板。路径/端口因机器而异，先自动检测（见仓库 AGENTS.md）
$env_out = python scripts/detect_env.py --export-ps1
Invoke-Expression ($env_out -join "`n")
& "$env:IDF_PATH\export.ps1"
idf.py set-target esp32s3
# 菜单中选中 CONFIG_BOARD_TYPE_WAVESHARE_S3_RLCD_4_2
idf.py menuconfig
idf.py build
idf.py -p $env:XIAOZHI_PORT flash monitor
```

---

## 变更记录

| 版本/提交 | 内容 |
|---|---|
| v3.3.1 | **BTSCAN 显示完整 BLE 设备信息**：地址类型（public/random）、事件类型（ADV/SCAN_RSP）、名称完整性、服务 UUID16/32/128、TX 功率、广播间隔、厂商数据（company ID + hex 载荷） |
| v3.3.0 | **蓝牙设备日志独立记录**：`ble_keyboard` TAG 日志（扫描/连接/断开/按键/HID）单独写入 `/sdcard/logs/ble_YYYYMMDD.txt`，不再混入系统日志；`SYSLOGLIST` 与 `self.list_chatlogs(system_logs)` 同步列出；HTTP `/logs/` 可浏览下载 |
| v3.2.2 | **修复启动期"检查新版本失败"红字**：OTA 检查前先短超时探测 DNS 就绪（消除 WiFi 刚关联时 getaddrinfo ~14s 超时导致的 `0x8001` 红字）；错误提示附带 `esp_err_to_name()` 可读错误名 |
| v3.2.1 | **修复开机"发送失败"（TLS 内存）**：`SSL_IN_CONTENT_LEN` 16384→8192、`SPIRAM_MALLOC_ALWAYSINTERNAL` 512→256、`DYNAMIC_FREE_PEER_CERT=y`，消除启动期 esp-aes DMA 内存耗尽；BLE 键盘两段式连接（BTSCAN 二次才连接，防 NimBLE 内存泄漏） |
| v3.2.0 | **新增蓝牙键盘输入（BLE HID Host）**：`BluetoothKeyboard` 类 + esp_hid（NimBLE）连接 BLE 键盘；快捷键控制（Enter=对话/Esc=停止/Space=监听/↑↓=音量/R=截图/T=录音/M=静音/V=版本/Tab=提示音 + 数字键快捷功能）；串口 `BTSCAN` 配对；修复自动扫描连接不可达键盘导致的内存泄漏 |
| 本次 | **新增对话日志（ChatLog）**：AI 对话文本+语音自动保存到 `/sdcard/logs/chatlogs/`，按时间+主题命名；`AudioService` 新增输入/输出音频 tap 回调；JSONL 文本 + 24kHz 双通道 WAV（麦克风+AI喇叭AEC回采）；串口 `CHATLOG` 调试命令 |
| 本次 | **新增 MP3 音乐播放**：`espressif/esp_audio_codec` 解码 + MCP 工具（list/play/stop/delete_music）+ 串口命令（MUSICLIST/MUSICPLAY/MUSICSTOP）+ `/sdcard/music` 目录 + 线性插值重采样链 |
| 本次 | **中文文件名支持**：FATFS 切换 UTF-8 API 编码 + codepage 936（简体中文），中文歌名可识别/播放 |
| 本次 | **修复音乐播放后语音失效**：新增 `ResumeAudioService()` 在音频服务重启后按设备状态恢复唤醒词/语音处理（`AudioService::Start()` 会清除唤醒词事件位且状态不切换时无人重启用） |
| 本次 | **播放通知**：播放音乐时底部通知栏显示歌名 / 采样率声道 / 播放进度（每 5s） |
| 本次 | 新增设备自检模块（`self.run_self_test` + `SELFTEST` 命令，7 项硬件测试）；I2C 句柄缓存、SD fsync 节流、ADC 健壮性等代码优化；console 切换为 USB-Serial/JTAG |
| `12822c6` | SD 卡录音/回放、SD 日志落盘（fsync）、MCP 音频工具、KEY 三击诊断 |
| `3409b22` | KEY 三功能按键、图标轮播显示、通知布局调整 |
| `4c4fb12` | 集成 PCF85063 RTC，开机即时时钟，NTP 后回写 |
| `20d0903` | 新增温湿度/电量 MCP 查询工具 |
| `9d0ca75` | 截图能力（串口 SHOOT + BOOT 长按 + 开机自动截图） |
| `35b51fe` | 初始移植：XiaoZhi AI v2.1.0 for waveshare-s3-rlcd-4.2 |
