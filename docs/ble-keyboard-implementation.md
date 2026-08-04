# 蓝牙键盘输入（BLE HID Host）完整配置与移植指南

> 版本：v3.3.4（2026-08-04）
> 目标板：waveshare-s3-rlcd-4.2（ESP32-S3）
> 覆盖：初始引入（v3.2.0）→ 完整设备信息（v3.3.1）→ 独立日志（v3.3.0/v3.3.2）→ 连接崩溃/双释放/持久化修复（v3.3.3/v3.3.4）

---

## 1. 功能概述

通过 **BLE HID Host**（`esp_hid` 组件 + NimBLE 协议栈）连接无线蓝牙键盘，作为设备输入设备。键盘按键映射为设备功能快捷键（对话控制/音量/截图/录音等）。

- **MVP 范围**：键盘作为"遥控器"触发设备已有功能（不改通信协议）
- **快捷键映射**：Enter=对话 / Esc=停止 / Space=监听 / ↑↓=音量 / R=截图 / T=录音 / M=静音 / V=版本 / Tab=提示音 / 数字键 1-9 快捷功能
- **配对**：Just Works（无 UI 自动配对），NVS 持久化（重启无需重新配对）
- **连接方式**：手动两段式 `BTSCAN`（扫描 → 再扫描连接），**无自动重连**（防内存泄漏的刻意设计）

---

## 2. 技术选型

| 项 | 选型 | 说明 |
|---|---|---|
| 协议栈 | **NimBLE**（`CONFIG_BT_NIMBLE_ENABLED`） | 比 Bluedroid 省 ~50KB 内存（S3 资源紧张） |
| HID Host | **`esp_hid` 组件**（IDF 自带） | `nimble_hidh.c` 官方维护，封装完整 HOGP 流程 |
| 键盘事件 | **`ESP_HIDH_INPUT_EVENT`** | 事件循环回调，`param->input.data` = HID 报告 |
| 配对 | **Just Works**（`sm_io_cap=NO_INPUT_OUTPUT`） | 无 UI 设备自动配对，NVS 持久化 |
| 芯片限制 | ESP32-S3 仅 BLE（无经典蓝牙） | 老式 BT 2.x/3.x 键盘无法配对 |

---

## 3. 文件清单

### 3.1 核心文件（新增 3 个）

| 文件 | 职责 |
|---|---|
| `main/boards/common/bluetooth_keyboard.h` | `BluetoothKeyboard` 类声明（公共 API + 状态字段） |
| `main/boards/common/bluetooth_keyboard.cc` | 完整实现（~550 行）：初始化/扫描/连接/报告解析/连接清理 |
| `main/workaround_bt_data.lf` | IDF 5.5.2 ldgen 'data' 段 bug 绕过（`MEM_ALLOC_MODE_EXTERNAL` 必需） |

### 3.2 修改文件（8 个）

| 文件 | 改动 |
|---|---|
| `sdkconfig.defaults.esp32s3` | +BLE/NimBLE/HID 配置（见 §4） |
| `main/Kconfig.projbuild` | +`USE_BLE_HID_KEYBOARD` 选项（见 §5） |
| `main/CMakeLists.txt` | +条件编译 bluetooth_keyboard.cc + LINKER_FRAGMENTS |
| 板 `waveshare-s3-rlcd-4.2.cc` | +成员/初始化/快捷键映射/BTSCAN 命令/独立日志分流 |
| `main/version_info.cc` | kFeatures[] +蓝牙键盘条目 |
| `CHANGELOG.md` | 版本条目 |
| `docs/usage.md` | 蓝牙键盘使用章节（配对/映射/断开） |
| 板 `README.md` | 串口命令表 +BTSCAN + 变更记录 |

---

## 4. sdkconfig 配置（sdkconfig.defaults.esp32s3）

```ini
# === BLE HID Host (Bluetooth keyboard input) ===
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_HID_SERVICE=y
CONFIG_BT_NIMBLE_ROLE_CENTRAL=y
# 2 connection slots: with 1, a leaked connection (from a failed GATT
# discovery) blocks every later connect with ESP_ERR_NO_MEM (0x101).
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2
CONFIG_BT_NIMBLE_SM_SC=y
CONFIG_BT_NIMBLE_SM_BONDING=y
CONFIG_BT_NIMBLE_NVS_PERSIST=y
# Use PSRAM for NimBLE heap to free internal RAM for audio/AFE.
# (workaround_bt_data.lf resolves the IDF 5.5.2 ldgen 'data' section bug
#  that previously blocked MEM_ALLOC_MODE_EXTERNAL.)
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y
# NimBLE host task on Core 0. Audio input task is pinned to Core 0 with
# priority 8 (> NimBLE), so audio is not starved; board tasks (SELFTEST,
# screenshot cmd) stay on Core 1. PSRAM handles the memory, so Core 0 is OK.
CONFIG_BT_NIMBLE_PINNED_TO_CORE=0
```

**关键配置解释**：

| 配置 | 值 | 为什么 |
|---|---|---|
| `BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL` | y | **必须**：NimBLE 堆放 PSRAM，否则占用 ~40KB 内部 RAM 导致 AFE 溢出/内存不足 |
| `BT_NIMBLE_MAX_CONNECTIONS` | 2 | **必须**：=1 时单条泄漏连接阻塞所有后续连接（崩溃根因之一） |
| `BT_NIMBLE_NVS_PERSIST` | y | 配对持久化（但**代码必须调用 `ble_store_config_init()`**，见 §6） |
| `BT_NIMBLE_PINNED_TO_CORE` | 0 | Core 0（音频任务优先级 8 > NimBLE，不饿死音频） |
| `BT_NIMBLE_HID_SERVICE` | y | HID 服务（虽然 Host 端实际由 esp_hid 处理） |

> ⚠️ **重要**：改 defaults 后需**同步 sdkconfig**（`idf.py reconfigure` 或手动改 sdkconfig 对应项）——defaults 只在新生成时生效，已有 sdkconfig 不会自动更新。

---

## 5. Kconfig 选项

```kconfig
config USE_BLE_HID_KEYBOARD
    bool "Bluetooth HID Keyboard (BLE HID Host)"
    depends on IDF_TARGET_ESP32S3
    default y
    select BT_ENABLED
    help
        Connect a BLE keyboard as input via esp_hid HID host (MVP: hotkeys).
```

- **默认开启**（`default y`），esp32s3 目标可用
- `select BT_ENABLED`：自动启用蓝牙总开关

---

## 6. 核心代码设计（bluetooth_keyboard.cc）

### 6.1 公共 API（bluetooth_keyboard.h）

```cpp
class BluetoothKeyboard {
public:
    using KeyCallback = std::function<void(uint8_t keycode, uint8_t modifier)>;
    using Callback = std::function<void()>;

    void Init();                              // 初始化（controller + esp_hidh + NimBLE host）
    void StartScan(uint32_t seconds = 10);    // 扫描 BLE 设备（appearance 0x03C1 过滤键盘）
    void Connect(const uint8_t* bda, uint8_t addr_type);  // 连接指定地址（阻塞，勿主线程调）
    void Disconnect();                        // 断开
    bool IsConnected() const { return dev_ != nullptr; }

    void OnKeyPress(KeyCallback cb);          // 按键回调
    void OnConnect(Callback cb);              // 连接回调
    void OnDisconnect(Callback cb);           // 断开回调
    static char KeycodeToAscii(uint8_t keycode, bool shift);  // HID 键码转 ASCII
};
```

### 6.2 初始化流程（Init → HostInit）

```
Init()
  ├─ esp_bt_controller_get_status()  // 若已初始化（如其他 BT 模块），跳过
  ├─ ControllerInit()                // BT controller（BLE only）
  ├─ esp_hidh_init(&config)          // esp_hid 事件循环（4096 栈）
  └─ HostInit()                      // NimBLE host 配置
       ├─ ble_hs_cfg.sync_cb / reset_cb / store_status_cb
       ├─ ble_hs_cfg.sm_io_cap = NO_INPUT_OUTPUT  // Just Works
       ├─ ble_hs_cfg.sm_bonding = 1
       ├─ ble_hs_cfg.sm_mitm = 0     // MUST be 0 for NO_INPUT_OUTPUT
       ├─ ble_hs_cfg.sm_sc = 1       // LE Secure Connections
       ├─ ble_store_config_init()    // ★★ NVS 持久化关键（见 6.4）
       └─ esp_nimble_enable(NimbleHostTask)
```

### 6.3 两段式扫描（防泄漏核心设计）

```
BTSCAN #1 → StartScan(10s)
  → BLE_GAP_EVENT_DISC 收集设备（appearance 0x03C1 = 键盘）
  → DISC_COMPLETE: 保存 pending_keyboard_addr_（不连接！）
  → 打印 "Pending keyboard saved; send BTSCAN again to connect"

BTSCAN #2 → StartScan(10s)
  → DISC_COMPLETE: has_pending_keyboard_ == true
  → ConnectAsync(pending_addr) → ConnectTask（独立任务）
      ├─ 内存预检: free_internal < 15000 → 中止（不崩溃）
      ├─ Connect() → esp_hidh_dev_open()（阻塞，GATT 发现）
      └─ 连接清理: ble_gap_conn_find_by_addr → 终止 stale 连接
```

**为什么两段式**：如果扫描完成自动连接，不可达键盘会导致 `esp_hidh_dev_open` 阻塞 30s + NimBLE 连接上下文泄漏（free heap 117KB→11KB，破坏 OTA 检查/HTTP 服务器）。两段式让用户**显式确认**连接意图。

### 6.4 配对持久化（NVS）——v3.3.4 修复的关键

```cpp
// bluetooth_keyboard.cc 顶部（CONFIG_BT_NIMBLE_ENABLED 内）
extern "C" void ble_store_config_init(void);   // ★ C 链接声明（libbt.a 是 C 符号）

// HostInit() 中，esp_nimble_enable 之前：
ble_store_config_init();  // ★ 把 store_read/write/delete_cb 接到 NVS 后端
```

**为什么必须**：`CONFIG_BT_NIMBLE_NVS_PERSIST=y` 只是编译支持，**运行时必须调用 `ble_store_config_init()`** 才把回调接到 NVS。否则：
- store 操作全部返回 `BLE_HS_ENOTSUP(8)`
- 启动日志：`Failed to restore IRKs from store; status=8`
- 配对密钥不写入 NVS → 重启后必须重新配对

**C 链接陷阱**：C++ 文件里普通 `extern` 声明会生成 C++ 修饰符号（`_Z21ble_store_config_initv`），链接不到 libbt.a 的 C 符号（`ble_store_config_init`）。必须用 `extern "C"`。

### 6.5 连接清理（v3.3.3 修复的崩溃根因）

```cpp
// ConnectTask 中，Connect() 返回后：
ble_addr_t peer;
memcpy(peer.val, args->bda, 6);
peer.type = args->addr_type;
struct ble_gap_conn_desc desc;
int rc = ble_gap_conn_find_by_addr(&peer, &desc);
if (rc == 0 && desc.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
    ble_gap_terminate(desc.conn_handle, 0x13);  // ★ 真实句柄
}
```

**原 bug**：`ble_gap_terminate(BLE_HS_CONN_HANDLE_NONE, ...)`（0xFFFF=无效句柄）永远失败 → 连接上下文泄漏 → 配合 `MAX_CONNECTIONS=1` 导致第二次连接 `ESP_ERR_NO_MEM(0x101)` 崩溃。现在用 `ble_gap_conn_find_by_addr` 查真实句柄。

---

## 7. 板级集成（waveshare-s3-rlcd-4.2.cc）

### 7.1 初始化 + 回调（L300-340）

```cpp
void InitializeKeyboard() {
    bt_keyboard_.Init();
    bt_keyboard_.OnKeyPress([this](uint8_t keycode, uint8_t modifier) {
        HandleKeyboardKey(keycode, modifier);  // 快捷键映射
    });
    bt_keyboard_.OnConnect([this]() {
        // 显示 "键盘已连接" 通知
    });
    bt_keyboard_.OnDisconnect([this]() {
        // 显示 "键盘已断开" 通知
    });
}

void KeyboardScanNow() {
    bt_keyboard_.StartScan(10);
}
```

### 7.2 快捷键映射（HandleKeyboardKey，L364-440）

```cpp
switch (keycode) {
    case 0x28: ToggleChat(); break;        // Enter
    case 0x29: StopListening(); break;     // Esc
    case 0x2C: StartListening(); break;    // Space
    case 0x52: ChangeVolume(+10); break;   // ↑
    case 0x51: ChangeVolume(-10); break;   // ↓
    case 0x13: TakeScreenshot(); break;    // R
    case 0x15: ToggleRecording(); break;   // T
    case 0x12: ToggleMute(); break;        // M
    case 0x19: ShowVersionPopup(); break;  // V
    case 0x2B: PlayTone(); break;          // Tab
    // 数字键 1-9: 快捷功能（提示音/配网/系统信息/录音/电量/HTTP/唤醒词/重启）
}
```

### 7.3 串口命令（ScreenshotCmdTask）

```cpp
} else if (strcmp(line, "BTSCAN") == 0) {
    board->KeyboardScanNow();   // 第一次=记录 pending，第二次=连接
}
```

### 7.4 独立蓝牙日志（v3.3.0 新增）

`ble_keyboard` TAG 的日志额外写入 `/sdcard/logs/ble_YYYYMMDD.txt`（与系统日志分离）。实现：`SdLogVprintf` 全局 tee 检测 TAG 分流。

---

## 8. 已知问题与修复历史（移植者必读）

### v3.2.0 → v3.3.4 修复清单

| 版本 | 问题 | 根因 | 修复 |
|---|---|---|---|
| v3.2.x | AFE 溢出 1636次/55s，空闲 RAM 12KB | NimBLE 栈占内部 RAM | `MEM_ALLOC_MODE_EXTERNAL` → PSRAM |
| v3.2.x | 扫描泄漏 ~17KB | 扫描完自动连接不可达键盘 | 两段式 BTSCAN |
| v3.2.1 | 开机"发送失败" | TLS DMA 内存耗尽 | `SSL_IN_CONTENT_LEN` 8192 + 释放对端证书 |
| v3.3.3 | 第二次连接崩溃 0x101 | `ble_gap_terminate(0xFFFF)` 无效句柄泄漏连接 | 真实句柄 + `MAX_CONNECTIONS=2` |
| v3.3.3 | Disconnect 双释放 | 显式断开 free + 异步 CLOSE_EVENT 再 free | CLOSE_EVENT 仅 dev_ 匹配时 free |
| v3.3.3 | 任务创建泄漏 | xTaskCreate 返回值未检查 | 失败即释放 |
| v3.3.4 | 配对不持久化 | 缺 `ble_store_config_init()` | 补调用（extern "C"） |

### 移植时最容易踩的坑

1. **忘调 `ble_store_config_init()`** → 配对每次重启丢失（`status=8`）
2. **`extern` 不带 `"C"`** → 链接失败（C++ 名字修饰）
3. **`MAX_CONNECTIONS=1`** → 一次连接失败后永久连不上（0x101 崩溃）
4. **不开 `MEM_ALLOC_MODE_EXTERNAL`** → AFE 溢出/内存不足（如果板子有 PSRAM）
5. **改 defaults 忘同步 sdkconfig** → 配置不生效
6. **`sm_mitm=1` 配 `NO_INPUT_OUTPUT`** → 配对失败（必须 0）

---

## 9. 移植步骤（新应用/新板子）

1. **拷贝核心文件**：`bluetooth_keyboard.{h,cc}` + `workaround_bt_data.lf`（若用 EXTERNAL 模式）
2. **配置 sdkconfig.defaults**：粘贴 §4 的 BLE 块
3. **加 Kconfig**：§5 的 `USE_BLE_HID_KEYBOARD`
4. **CMakeLists**：条件编译 bluetooth_keyboard.cc + `LINKER_FRAGMENTS` 注册 workaround
5. **板集成**：加 `bt_keyboard_` 成员 + `InitializeKeyboard()` + 快捷键映射 + BTSCAN 命令
6. **同步 sdkconfig**：`idf.py reconfigure` 或手动改
7. **验证**：
   - 启动无 `status=8`（持久化生效）
   - BTSCAN 两次能连接键盘
   - 重启后 BTSCAN（不用重新配对）
   - 连接失败不崩溃（内存预检）

---

## 10. 待办/可扩展方向

- **自动重连**：目前手动 BTSCAN（防泄漏设计）。如需自动，可在 `sync_cb` 后读取 NVS 已配对 MAC 尝试一次连接（非扫描，失败静默）
- **文本注入**：键盘输入文本到对话（需 Protocol 层 `type:stt` 扩展 + 服务器支持）
- **多键盘管理**：`MAX_BONDS=3` 已支持多设备，但当前 UI 只管理一个
