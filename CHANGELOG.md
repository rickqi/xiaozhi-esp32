# 变更记录 (CHANGELOG)

本文件记录固件版本变更历史。每个版本按时间倒序排列，包含变更摘要与新功能/修复/变更分类。

> **规则**：每次版本 bump 必须更新本文件（见 AGENTS.md 版本管理章节）。

---

## v3.4.4 — 2026-08-04

### fix
- **验证"连接成功 30-40 秒后重启"根因（SMP 配对超时）**：
  - 临时禁用 LE Secure Connections 配对（`sm_bonding=0, sm_sc=0`，标记 TEMP VERIFICATION）——验证假设
  - `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` 4096→8192（防 SC 配对 ECDH 栈溢出）
  - 实测：连接后持续稳定（uptime 3 小时无崩溃），30-40s 重启消失 → 确认 SMP 配对超时是根因
- **注意**：禁配对为临时验证，最终方案待定（保持禁配对 / 恢复配对修 own_addr_type / 恢复配对优雅处理超时）

---

## v3.4.3 — 2026-08-04

### fix
- **扫描期间蓝牙日志降噪（防崩溃）**：`ble_keyboard` TAG 日志在 GAP 扫描期间跳过 SD 卡写入（仅 console 输出）——避免 NimBLE 任务被每次广播的 SD `fwrite+fflush` 阻塞，导致 HCI 缓冲溢出、控制器重置、设备重启（多次 BTSCAN 后重启的根因之一）
  - 新增 `BluetoothKeyboard::IsScanning()` 状态（StartScan 置 true，DISC_COMPLETE 置 false）
  - `SdLogVprintf` 检测扫描状态，扫描期间跳过 BLE 独立日志的 SD 写入

---

## v3.4.2 — 2026-08-04

### fix
- **修复蓝牙键盘连接成功后立即被断开**（多次扫描后重启的根因之一）：
  - 根因：`ConnectTask` 在 `Connect()` 返回后**无条件**执行 stale 连接清理——连接成功时 `ble_gap_conn_find_by_addr` 也会找到刚建立的连接并 `ble_gap_terminate` 杀掉（实测日志：`OPEN: MIIIW MECH-KB Pro` → `BLE keyboard connected` → `Terminating stale BLE connection handle=1`）
  - 修复：仅当 `Connect()` **失败**（`dev_ == nullptr`）时才清理 stale 连接；成功时保留活动连接（打印 `Connect success: keeping active BLE connection`）
  - 效果：键盘连接后持续保持（不再 200ms 后断开），消除反复连接/断开循环

---

## v3.4.1 — 2026-08-04

### feat
- **语音命令扫描蓝牙（MCP 工具 `self.scan_ble`）**：AI 对话可直接触发蓝牙扫描（替代仅串口 BTSCAN）
  - 描述引导 AI：先让键盘进配对模式 → 调用一次扫描 → 发现键盘后再调用一次连接
  - 实现：`InitializeTools()` 注册 `self.scan_ble`（无参），直接复用 `KeyboardScanNow()`（含 SPINNER 图标更新 + `StartScan(10)`）
  - 线程安全：复用现有路径（MCP 回调 → KeyboardScanNow → Schedule 处理 UI 图标）

---

## v3.4.0 — 2026-08-04

### feat
- **状态栏蓝牙图标**：顶部状态栏 WiFi 图标旁新增 BLE 状态图标，随蓝牙状态变化
  - 已连接：显示蓝牙图标（`FONT_AWESOME_BLUETOOTH`）
  - 扫描中：显示旋转图标（`FONT_AWESOME_SPINNER`）
  - 断开/未连接：隐藏图标
  - 实现：`LcdDisplay::SetBluetoothIcon()`（Display 抽象类 + LcdDisplay 实现）；`InitializeKeyboard()` 的 OnConnect/OnDisconnect 回调 + `KeyboardScanNow()` 驱动图标更新（经 `Application::Schedule` 主线程安全更新）
  - 布局：WiFi + 蓝牙图标包装在 `left_icons` flex 容器（与右侧 mute/battery 对称），`SPACE_BETWEEN` 保持左侧成组
  - 已验证：截图确认 WiFi 图标旁蓝牙标志正常渲染

---

## v3.3.4 — 2026-08-04

### fix
- **修复蓝牙键盘配对不持久化**（重启后需重新配对）：`HostInit()` 未调用 `ble_store_config_init()`，导致 `ble_hs_cfg` 的 store 回调为 NULL——即使 `CONFIG_BT_NIMBLE_NVS_PERSIST=y`，绑定密钥也不写入 NVS（store 操作全部返回 `BLE_HS_ENOTSUP`，启动日志出现 `Failed to restore IRKs from store; status=8`）
  - 修复：`HostInit()` 在 `esp_nimble_enable()` 前调用 `ble_store_config_init()`（`extern "C"` 声明，匹配 libbt.a 的 C 符号）
  - 效果：键盘配对后重启仍保留绑定信息，无需重新配对

---

## v3.3.3 — 2026-08-04

### fix
- **修复蓝牙键盘第二次连接崩溃（ESP_ERR_NO_MEM 0x101）**：首次连接部分成功后 NimBLE 连接上下文因 `ble_gap_terminate` 使用无效句柄（0xFFFF）泄漏 → `MAX_CONNECTIONS=1` 时第二次连接无空槽 → 崩溃
  - `ConnectTask` 改用 `ble_gap_conn_find_by_addr()` 查找真实句柄再 terminate
  - 连接前内存预检（内部 RAM <15KB 时优雅中止，不再崩溃）
  - `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` 1→2（单条泄漏连接不再阻塞后续连接）
- **修复 Disconnect() 双释放**：显式断开路径 `esp_hidh_dev_close` + 立即 free + 异步 CLOSE_EVENT 再次 free（堆损坏）——CLOSE_EVENT 仅在 `dev_` 仍匹配时 free
- **修复 ConnectAsync 任务创建泄漏**：`xTaskCreate` 返回值未检查，失败时泄漏 `addr`/`args`——现在失败即释放并打印错误

---

## v3.3.2 — 2026-08-04

### feat
- **串口 `LOG BLE` 命令**：读取独立蓝牙日志文件（`/sdcard/logs/ble_YYYYMMDD.txt`），与 `LOG`（系统日志）对应，便于串口直接排查键盘扫描/连接/按键事件
  - 实现：`DumpSdLog(ble_log)` 支持目标文件选择；`LOG BLE`/`LOGBLE` 命令触发；BLE 日志路径动态构造（懒加载未触发时也能读取）

---

## v3.3.1 — 2026-08-04

### feat
- **BTSCAN 扫描显示完整 BLE 设备信息**：除原有 MAC/名称/RSSI/appearance 外，新增
  - 地址类型（public/random/rpa-pub/rpa-rnd）— 判断设备是否使用随机地址（隐私保护）
  - 事件类型（ADV_IND/SCAN_RSP/NONCONN_IND 等）— 区分初始广播包与扫描响应，解释设备名为何常为空
  - 名称完整性标记（complete/incomplete）
  - 服务 UUID16/32/128 全打印（含 HID 0x1812 识别）
  - TX 功率（dBm）
  - 广播间隔（ms）
  - **厂商数据**（mfg data）：company ID + 完整 hex 载荷（如 company=0x004c = Apple），便于识别设备厂商/型号

---

## v3.3.0 — 2026-08-04

### feat
- **蓝牙设备日志独立记录**：BLE 键盘相关日志（扫描/连接/断开/按键/HID 报告）不再混入系统日志，单独写入 `/sdcard/logs/ble_YYYYMMDD.txt`
  - 实现：`SdLogVprintf`（全局 ESP_LOG tee）按 `ble_keyboard` TAG 分流，额外追加到独立文件；懒加载打开（SD 未就绪时自动重试）；独立 fsync 节流（1s）
  - 系统日志列表（串口 `SYSLOGLIST` 与 MCP `self.list_chatlogs(system_logs)`）同步识别 `ble_` 前缀文件
  - HTTP 文件服务器 `/logs/` 目录已自动暴露独立蓝牙日志文件（可浏览/下载）

---

## v3.2.4 — 2026-08-04

### fix
- **修复音乐播放无声**（"播放危险派对/任何歌曲都没声音"）：
  - 根因：`PlayMusicByName()` 用 `xTaskCreatePinnedToCore` 动态创建 12KB 内部 RAM 任务栈；内部 RAM 被 BLE heap + TLS 缓冲 + 音频任务碎片化后分配失败（返回 -1），且**返回值未检查** → 函数静默返回 true（MCP 报 "Playing music"）但播放任务从未运行 → 无声无日志
  - 修复：改为**常驻音乐播放任务**（`MusicTask()`，创建一次）+ **PSRAM 静态栈**（16KB，`heap_caps_malloc(MALLOC_CAP_SPIRAM)`）——不受内部 RAM 碎片影响；播放请求经二进制信号量传递，任务循环复用栈/TCB
  - 顺带修复：任务创建失败现在会返回 false + 打印错误日志（MCP 报 "file not found" 而非虚假成功）
- **BOOT 三击版本弹窗文本缓冲溢出警告**：`char info[512]` 不足以容纳扩充后的版本摘要（`-Werror=format-truncation` 报错），增至 640

---

## v3.2.3 — 2026-08-04

### fix
- **版本信息与实际功能一致性修正**（`self.get_version_info` 语音查询内容校准）：
  - `kFeatures[]` 第 2 条移除错误的"4G"声明（本板 waveshare-s3-rlcd-4.2 仅 WiFi，无 ML307 模块）
  - `kFeatures[]` 第 4 条移除错误的"LED"声明（本板无 LED，工具列表为音箱/屏幕/温湿度/电量/录音/音乐/日志）
  - 移除冗余的 `git_commit_full` JSON 字段（与 `git_commit` 同值且无消费方，构建系统仅注入短 hash）
- **BOOT 三击版本弹窗"近期版本更新"摘要滞后**：补充 v3.2.0（蓝牙键盘输入）、v3.1.0（环境自检·TLS 优化）条目，与实际功能一致

### change
- 板 README 变更记录表补充 v3.2.1/v3.2.2 条目（TLS 内存修复、DNS 就绪等待）

---

## v3.2.2 — 2026-08-04

### fix
- **消除启动期"检查新版本失败"红字提示**：WiFi 刚关联时 DHCP/DNS 尚未就绪，`CheckVersion()` 内的 `getaddrinfo()` 会阻塞 ~14s 后返回 `0x8001`（`ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME`），导致每次开机都弹红色错误横幅
  - `CheckNewVersion()` 在发起 OTA 检查前先用短超时探测 OTA 域名 DNS（最多 6 次 × 2s），解析成功后才调用 `CheckVersion()`；DNS 始终不可达则静默放弃（不打搅用户）

### change
- **错误提示可读性**：OTA 检查失败提示由裸数字 `code=32769` 改为 `code=32769 (ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME)`，便于定位问题

---

## v3.2.1 — 2026-08-04

### fix
- **修复开机"发送失败"（TLS 内存分配失败）**：启动早期（AFE 初始化 + WiFi 连接 + MQTT 激活并发峰值）内部 DMA RAM 耗尽，导致 `esp-aes: Failed to allocate memory` → MQTT/HTTP 发送失败
  - `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` 16384→8192：硬件 AES DMA 暂存缓冲与记录大小成正比，减半后大幅降低启动期 DMA 内存需求
  - `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 512→256：减少小分配挤占内部 DMA 池，降低碎片化
  - `CONFIG_MBEDTLS_DYNAMIC_FREE_PEER_CERT=y`：握手后释放对端证书（省 1-2KB）
- **BLE 键盘两段式连接**：BTSCAN 第一次只记录键盘地址，第二次才连接——避免每次扫描自动连接不可达键盘泄漏 ~17KB NimBLE 内存
- **BTSCAN 调试日志**：StartScan/DISC_COMPLETE 打印 pending 状态（诊断用）

### change
- 设备启动期内存峰值优化：通过 TLS 缓冲调优 + PSRAM 分散分配，启动不再因内存不足导致发送失败

---

## v3.2.0 — 2026-08-03

### feat
- **蓝牙键盘输入（BLE HID Host）**：通过 `esp_hid` 组件（NimBLE 后端）连接 BLE 键盘，作为输入设备
  - 新增 `BluetoothKeyboard` 类（`main/boards/common/bluetooth_keyboard.{h,cc}`）：esp_hidh 初始化、GAP 扫描（appearance 0x03C1 过滤键盘）、HID Boot 报告解析、Just Works 配对（无 UI 设备）
  - **快捷键映射**：Enter=开始/停止对话、Esc=停止监听、Space=开始监听、↑/↓=音量±10、R=截图、T=录音、M=麦克风静音、V=版本信息、Tab=提示音
  - **数字键快捷功能**：1=提示音、2=重置网络、3=系统信息、4=播放最近录音、5=电量、6/7=HTTP 服务器开关、8=唤醒词触发、9=重启
  - **串口命令 `BTSCAN`**：手动触发 BLE 键盘扫描配对（避免开机自动扫描导致的内存泄漏）
  - 屏幕通知：键盘连接/断开时显示"键盘已连接/已断开"
- **Kconfig 选项 `USE_BLE_HID_KEYBOARD`**：esp32s3 目标默认开启，`select BT_ENABLED` + NimBLE 配置

### fix
- **BLE 连接内存泄漏**：开机自动扫描连接不可达键盘会泄漏 NimBLE 内存（free RAM 117KB→11KB，导致"检查新版本失败"和 HTTP 服务器启动失败）。修复：改为 `BTSCAN` 手动触发 + 连接失败清理 stale bond + 连接任务看门狗清理
- **sdkconfig 漂移**：清理环境迁移残留的 esp32 配置，重新生成 esp32s3 目标（含 BLE HID 配置）

### change
- `sdkconfig.defaults.esp32s3`：新增 BLE/NimBLE/HID 配置（`BT_ENABLED`、`BT_NIMBLE_ENABLED`、`BT_NIMBLE_HID_SERVICE`、`ROLE_CENTRAL`、SM_SC/BONDING/NVS_PERSIST 等）
- `main/version_info.cc` `kFeatures[]`：新增蓝牙键盘输入功能说明

---

## 开发环境工具 — 2026-08-03（不涉及固件版本）

> 纯开发工具/文档变更，**不 bump 固件版本**（`PROJECT_VER` 仍为 3.1.0），仅为记录环境自检与监控能力建设。

### feat（开发工具）
- **新增 `scripts/detect_env.py` 多环境自检工具**：解决环境迁移后 AGENTS.md/文档中硬编码路径（COM 口、IDF 路径、Python env）失效的问题
  - `detect_env.py`（默认）：人类可读摘要——IDF 路径/版本、Python 虚拟环境、设备串口、项目版本、git 提交
  - `--json`：结构化输出（供脚本/agent 消费）
  - `--check`：与 `scripts/env_expect.json` 快照对比，判定环境冲突（换机/IDF 重装/串口变化 → exit 2）
  - `--health`：**10 项健康检查**，验证环境是否满足开发/调试/烧录要求（任一 FAIL → exit 2）
  - `--export-ps1`：生成 PowerShell 环境变量赋值（`$env:IDF_PATH` / `$env:XIAOZHI_PORT` 等），一键激活
- **新增 `scripts/env_expect.json` 环境快照基准**：记录"上次验证"的机器环境，供 `--check` 冲突判定
- **检测机制**：IDF 路径优先级（`IDF_PATH` 环境变量 > `~/.espressif/idf-env.json` 安装器元数据 > 常见路径探测）；串口优先识别 Espressif USB-Serial/JTAG（VID `0x303A`）

### change（文档/监控要求）
- **AGENTS.md 构建环境章节重写**：环境信息不再硬编码，改为检测驱动标准工作流（检测 → 导出变量 → 激活 → 构建/烧录/监控）
- **新增 sdkconfig 漂移陷阱警示**：环境迁移后 `sdkconfig` 可能残留其他目标/板的旧配置（如 esp32+bread-compact 而本板是 esp32s3+waveshare-s3-rlcd-4.2），`idf.py build` 会"成功"但编译错误板——必须先用 `--health` 确认 `sdkconfig 芯片/板` 项 PASS
- **文档去硬编码**：`docs/usage.md`、`docs/optimization-plan.md`、板 `README.md`、`CHANGELOG.md` 中所有 `COM4` / 绝对路径替换为检测驱动命令
- **串口监控要求（不更新固件）**：通过 COM 实时日志（温湿度/系统事件/错误）、串口命令（`SELFTEST`/`LOG`/`SYSLOGLIST`/`CHATLOGLIST`/`LIST`/`HTTPSTART`）、HTTP 远程监控（同网段，10 分钟自动关闭）三种方式监控设备状态；判定正常基准见 AGENTS.md

---

## v3.1.0 — 2026-08-03

### feat
- **BOOT 三击全屏版本信息弹窗**：独立 LVGL 全屏覆盖（400×300 白底），14px 字体居中显示版本号+git 提交+构建时间+近期变更摘要（v2.5→v3.0）+按键说明，15 秒自动消失，重复三击刷新
- **LvglDisplay::SetNotificationFont()**：动态切换通知字体，定时器到期自动恢复默认字体

### fix
- **BOOT 单击日志补齐**：所有 8 项按键手势现在都有 ESP_LOGI 输出

---

## v3.0.0 — 2026-08-03 ⚠️ MAJOR（分区变更，需 USB 烧录）

### change
- **分区表调整**：OTA 槽 3.94MB → 4.63MB（+696KB/槽），Assets 8MB → 6.63MB（-1.37MB，仍剩 4MB）
  - ota_0/ota_1: `0x3F0000` → `0x4A0000`
  - assets: 偏移 `0x800000` → `0x960000`，大小 `8M` → `0x6A0000`
- **不兼容 OTA**：v2.x 无法 OTA 到 v3.0.0（分区地址变了），**必须通过 USB 烧录**

### feat
- **BOOT 三击版本信息全屏弹窗**：14px 小字体、白底黑字全屏覆盖（不与表情/状态栏重叠）、居中显示版本号+git 提交+近期变更摘要+按键说明，15 秒自动消失
- **LvglDisplay::SetNotificationFont()**：支持动态切换通知字体
- **BOOT 单击日志补齐**：所有 8 项按键手势现在都有 ESP_LOGI 输出

### 烧录步骤
```powershell
# 端口/路径因机器而异，先自动检测（见 AGENTS.md 构建环境章节）
$env_out = python scripts/detect_env.py --export-ps1
Invoke-Expression ($env_out -join "`n")
& "$env:IDF_PATH\export.ps1"
idf.py -p $env:XIAOZHI_PORT erase-otadata
idf.py -p $env:XIAOZHI_PORT flash
```

---

## v2.9.0 — 2026-08-02

### feat
- **传感器历史记录**：每 5 分钟自动采样温湿度/电量到 `/sdcard/sensors/sensor_log.csv`，HTTP `/sensors` 页面查看历史表格
- **定时器/闹钟**：MCP 工具 `self.set_timer`（duration_minutes + message），到时播放提示音 + 通知

### change
- HTTP 下载 buffer 8KB → 16KB（吞吐量提升）

---

## v2.8.0 — 2026-08-02

### feat
- **WiFi 配置热更新**：HTTP 页面 `/wifi` 修改 WiFi SSID/密码，提交后自动断开重连（使用 SsidManager 持久化到 NVS）
- **OTA 在线固件升级**：HTTP 页面 `/upload` 上传 .bin 固件文件，设备自动写入备用 OTA 槽并重启（`esp_ota_*` API 流式写入，PSRAM 缓冲，进度日志）
- 首页增加 WiFi 配置和固件升级入口

---

## v2.7.0 — 2026-08-02

### feat
- **mDNS 设备发现**：设备注册为 `xiaozhi.local`，手机直接访问 `http://xiaozhi.local/`（无需查 IP）
- **WAV 在线播放**：目录列表对 .wav 文件显示内联 `<audio>` 播放器，浏览器直接播放录音/chatlog
- **ChatLog 在线查看**：新增 `/view/<path>` 路由，解析 JSONL 渲染为聊天气泡 HTML（用户蓝/小智灰，最多 100 轮）
- **文件删除**：目录列表每项加 🗑️ 按钮，JS fetch POST 删除后自动刷新
- **HTTP 服务器自动关闭**：10 分钟无访问自动 Stop（释放 8KB 栈 + socket 资源）
- 新增依赖 `espressif/mdns ^1.4.0`（自动下载 v1.11.3）

---

## v2.6.1 — 2026-08-02

### fix
- **HTTP 文件服务器优化修复**：
  - httpd 栈 4096→8192（防止目录浏览时栈溢出）
  - recv/send 超时 5s→10s/30s（防止大文件传输超时）
  - 添加 Content-Length 响应头（浏览器显示文件总大小和下载进度）
  - ServeFile/WildcardHandler 增加诊断日志（fopen/size/进度/错误追踪）

---

## v2.6.0 — 2026-08-02

### feat
- **BOOT 三击显示版本信息**：三击 BOOT 键显示固件版本（含 git 提交、构建时间）+ 按键功能分布简明说明，8 秒通知

---

## v2.5.0 — 2026-08-02

### feat
- **WiFi HTTP 文件服务器**：通过浏览器/curl 无线下载 SD 卡文件（日志/对话日志/录音/音乐），规避串口冲突
  - 新增 `main/http_file_server.{h,cc}`：`HttpFileServer` 单例（`Start(port)`/`Stop()`/`GetUrl()`）
  - 路由：`GET /` 首页（SD 卡目录导航）/ `GET /status` JSON 状态 / `GET /*` 通配（目录浏览 HTML + 文件下载流式传输）
  - 文件流式传输用 PSRAM 缓冲（8KB chunk），Content-Type 自动识别（txt/wav/mp3/json 等）
  - 路径遍历防护（拒绝 `..`）
- 新增 MCP 工具 `self.start_file_server` / `self.stop_file_server`（语音可调用，返回 URL）
- 新增串口命令 `HTTPSTART` / `HTTPSTOP`
- 无新依赖（`esp_http_server` 已链接进固件）

---

## v2.4.0 — 2026-08-02

### feat
- **list_chatlogs 支持系统日志目录**：`self.list_chatlogs` 新增可选 `directory` 参数（`chatlogs` 默认 / `system_logs` / `logs`），可语音查询 `/sdcard/logs/` 下的系统日志文件（`log_YYYYMMDD.txt`）
- 新增 `ListSystemLogs()` / `ListSystemLogsJson()` 辅助函数
- 新增 `SYSLOGLIST` 串口命令（列系统日志）

---

## v2.3.0 — 2026-08-02

### feat
- **ChatLog 目录管理 MCP 工具**（4 个语音可调用工具）：
  - `self.list_chatlogs`：列出最近 10 条对话（name/topic/大小/时长/真实开始时间）
  - `self.get_chatlog_summary`：解析 JSONL 返回对话摘要（最多 50 轮）
  - `self.play_chatlog_audio`：播放对话音频，声道可切换（mixed/mic/ai）
  - `self.delete_chatlog`：删除对话（.txt + 配对 .wav）
- 新增 `CHATLOGLIST` 串口命令
- 新增 8 个辅助函数：`BuildChatlogPath`、`TopicFromChatlogName`、`ListChatlogs(+Json)`、`GetChatlogSummaryJson`、`PlayChatlogPath`（声道可切换 + 可中断）、`PlayChatlogByName`、`DeleteChatlogByName`
- 播放守卫 `chatlog_playing_` 与 `music_playing_` 互斥

---

## v2.2.0 — 2026-08-02

### feat
- **固件版本信息 MCP 工具**：`self.get_version_info`（语音可调用），返回版本号、构建时间、git 提交、IDF 版本、ELF SHA、分区表、功能清单、烧录指引
- 新增 `main/version_info.{h,cc}`：`VersionInfo::GetVersionString()` / `GetFeatureListJson()` / `BuildVersionInfoJson()`
- 新增 `GIT_COMMIT` 宏捕获（`main/CMakeLists.txt` 中 `git rev-parse --short HEAD`，失败回退 `unknown`）
- 更新 `docs/usage.md` 与板 README MCP 工具表

### fix
- **C1 通知空闲时钟抢占修复**：`ShowNotification()` 重置 `last_status_update_time_` 使空闲时钟窗口重开；`UpdateStatusBar()` 空闲时钟分支在通知可见时跳过 `SetStatus(时钟)`——短通知（3s）与长通知（30s）均可完整显示满 duration_ms
- 新增 `docs/notification-display-analysis.md` 分析报告

---

## v2.1.0 — 基线版本

### feat（本分支定制功能线）
- AAC/M4A 音乐支持、中文主题文件名、线程安全时间
- 对话日志落盘（ChatLog）
- MP3 音乐播放、硬件自检、中文文件名支持、音频服务修复
- SD 卡录音/回放、SD 日志 tee（fsync 节流）、MCP 音频工具、KEY 三击诊断
- KEY 三功能按键、图标轮播显示、通知布局调整
- PCF85063 RTC 集成（开机即时时钟，NTP 回写）
- 温湿度/电量 MCP 查询工具
- 截图能力（串口 SHOOT + BOOT 长按 + 开机自动截图）
