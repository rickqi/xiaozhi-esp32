# 蓝牙键盘输入（BLE HID Host）功能说明与移植指南

> 版本：v3.2.0（commit 9d377fb / 8da711d）
> 目标板：waveshare-s3-rlcd-4.2（ESP32-S3）
> 文档日期：2026-08-03

---

## 1. 功能概述

通过 **BLE HID Host**（`esp_hid` 组件 + NimBLE 协议栈）连接无线蓝牙键盘，
作为设备的输入设备。键盘按键映射为设备功能快捷键（对话控制/音量/截图/录音等）。

- **MVP 范围**：键盘作为"遥控器"触发设备已有功能（不改通信协议）
- **后续扩展**：文本注入（需新增 Protocol 方法 + 服务器支持 `type:stt`）

---

## 2. 技术选型

| 层 | 选型 | 说明 |
|---|---|---|
| 协议栈 | **NimBLE**（`CONFIG_BT_NIMBLE_ENABLED`） | 比 Bluedroid 省 ~50KB 内存（S3 资源紧张） |
| HID Host | **`esp_hid` 组件**（IDF 自带） | `nimble_hidh.c` 官方维护，封装完整 HoGP 流程 |
| 键盘事件 | **`ESP_HIDH_INPUT_EVENT`** | 事件循环回调，`param->input.data` 为 HID 报告 |
| 配对 | **Just Works**（`sm_io_cap=NO_INPUT_OUTPUT`） | 无 UI 设备自动配对，NVS 持久化 |
| 芯片限制 | ESP32-S3 仅 BLE（无经典蓝牙） | 老式 BT 2.x/3.x 键盘无法配对 |

---

## 3. 文件清单

### 3.1 新增文件（3 个）

| 文件 | 职责 |
|---|---|
| `main/boards/common/bluetooth_keyboard.h` | `BluetoothKeyboard` 类声明（98 行） |
| `main/boards/common/bluetooth_keyboard.cc` | 完整实现（414 行）：初始化/扫描/连接/报告解析 |
| `main/workaround_bt_data.lf` | IDF 5.5.2 ldgen bug 的 workaround（见 §6） |

### 3.2 修改文件（9 个）

| 文件 | 改动 |
|---|---|
| `sdkconfig.defaults.esp32s3` | +BLE/NimBLE/HID 配置（见 §4） |
| `main/Kconfig.projbuild` | +`USE_BLE_HID_KEYBOARD` 选项（见 §5） |
| `main/CMakeLists.txt` | +条件编译 bluetooth_keyboard.cc + LINKER_FRAGMENTS |
| `main/boards/waveshare-s3-rlcd-4.2/waveshare-s3-rlcd-4.2.cc` | +成员/初始化/映射表/BTSCAN 命令 |
| `main/version_info.cc` | kFeatures[] +蓝牙键盘条目 |
| `CHANGELOG.md` | +v3.2.0 条目 |
| `docs/usage.md` | +第5章蓝牙键盘（配对/映射/断开）|
| 板 `README.md` | 串口命令表 +BTSCAN + 变更记录 |
| `CMakeLists.txt` | PROJECT_VER 3.1.0 → 3.2.0 |

---

## 4. sdkconfig 配置（sdkconfig.defaults.esp32s3）

```ini
# === BLE HID Host (Bluetooth keyboard input) ===
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_HID_SERVICE=y
CONFIG_BT_NIMBLE_ROLE_CENTRAL=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
CONFIG_BT_NIMBLE_SM_SC=y
CONFIG_BT_NIMBLE_SM_BONDING=y
CONFIG_BT_NIMBLE_NVS_PERSIST=y
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y
CONFIG_BT_NIMBLE_PINNED_TO_CORE=1
```

**关键项说明**：
- `BT_NIMBLE_HID_SERVICE=y`：**必须**，否则 `nimble_hidh.c` 整文件被 `#if` 编译排除
  （注意：它强制启用 GATT server，不可关闭 GATT_SERVER 省内存）
- `BT_NIMBLE_ROLE_CENTRAL=y`：中心角色（Host，主动连接键盘）
- **`MEM_ALLOC_MODE_EXTERNAL=y`：必须**——NimBLE 堆放 PSRAM，
  否则协议栈（~40KB）挤占内部 RAM 导致音频 AFE 环形缓冲溢出
  （实测：INTERNAL 时 AFE 溢出 1636 次/55s、空闲内存 12KB；
  EXTERNAL 后 AFE 溢出 ~0、空闲内存 37KB）
- **`PINNED_TO_CORE=1`：建议**——NimBLE 主机任务固定 Core 1，
  避免与音频任务（Core 0）争抢 CPU
- `BT_NIMBLE_HID_SERVICE` + `MEM_ALLOC_MODE_EXTERNAL` 依赖
  `main/workaround_bt_data.lf` 修复的 ldgen bug（见 §6）

> ⚠️ 不要参考 Waveshare FactoryProgram 的 `BT_ABORT_WHEN_ALLOCATION_FAILS` /
> `BT_BLE_42_FEATURES_SUPPORTED`——它们 `depends on BT_BLUEDROID_ENABLED`，对 NimBLE 无效。

---

## 5. Kconfig 选项（main/Kconfig.projbuild）

```kconfig
config USE_BLE_HID_KEYBOARD
    bool "Bluetooth HID Keyboard (BLE HID Host)"
    depends on IDF_TARGET_ESP32S3
    default y
    select BT_ENABLED
    help
        Connect a BLE keyboard as input via esp_hid HID host (MVP: hotkeys).
```

- `select BT_ENABLED`：启用 BT 总开关（NimBLE 的 choice 由 defaults 设置）
- `depends on IDF_TARGET_ESP32S3`：BLE 芯片限制（C3/C6 也支持 BLE，可放宽）

---

## 6. IDF 5.5.2 ldgen bug 及 workaround

**问题**：`components/bt/linker_rw_bt_controller.lf` 的 `[scheme:bt_default]` 无条件
引用 `data -> dram0_data`，但 ESP32-S3 的 `sections.ld.in` 无独立 `data` 段
（用 `dram0_data`），导致完整构建时 ldgen 报
`ERROR: Undefined reference to sections 'data'`。

**workaround**（`main/workaround_bt_data.lf`）：
```
[sections:data]
entries:
    .data
```
在 `main/CMakeLists.txt` 的 `idf_component_register` 中注册：
```cmake
LINKER_FRAGMENTS workaround_bt_data.lf
```

> **为何必须**：没有它，`MEM_ALLOC_MODE_EXTERNAL`（NimBLE→PSRAM，修复 AFE 溢出
> 的关键配置）无法通过 ldgen。workaround 使 EXTERNAL 模式可用。
定义 `data` 段别名使 scheme 可解析。

**环境依赖陷阱**（跨机器构建注意）：
- **pyparsing 3.2.0**：ldgen 兼容版本（3.1.2/3.2.1/3.2.5 均与 ldgen 有兼容问题）
- **ruamel.yaml 0.18.6**：component manager 兼容版本（0.19.x 报 `TypeError`）
- 换环境后若 ldgen 崩溃，先检查这两个包版本

---

## 7. 代码架构

### 7.1 类设计（bluetooth_keyboard.h）

```
BluetoothKeyboard
├── Init()          // esp_hidh_init + BT controller + NimBLE host（幂等）
├── StartScan()     // GAP 扫描，appearance 0x03C1 过滤键盘
├── Connect()       // esp_hidh_dev_open（阻塞，独立任务调用）
├── Disconnect()    // 断开 + 释放
├── IsConnected()   // 查询状态
├── OnKeyPress()    // 按键回调（仿 Button 类 std::function 模式）
├── OnConnect()     // 连接回调
├── OnDisconnect()  // 断开回调
└── KeycodeToAscii() // 静态工具：HID keycode → ASCII
```

### 7.2 初始化流程（Init → 事件链）

```
Init()
├── esp_bt_controller_init/enable(BLE)     // 若控制器空闲
├── esp_nimble_init()                       // NimBLE host
├── esp_hidh_init(config)                   // 注册 EventHandler
├── HostInit()                              // ble_hs_cfg + esp_nimble_enable
│   ├── sm_io_cap = NO_INPUT_OUTPUT         // Just Works
│   ├── sm_bonding = 1, sm_mitm = 0         // 无 UI 配对
│   └── sm_sc = 1                           // LE Secure Connections
└── ESP_LOGI("BLE HID host ready")

BTSCAN → StartScan(10s)
└── ble_gap_disc() → GapEventCallback
    ├── BLE_GAP_EVENT_DISC: 解析 adv fields（名称/appearance/UUID）
    │   └── appearance==0x03C1 → 记录键盘地址
    └── BLE_GAP_EVENT_DISC_COMPLETE → ConnectAsync（独立任务）
        └── Connect() → esp_hidh_dev_open()（阻塞，含 GATT 发现+MTU）
            ├── 成功 → dev_ = dev → OPEN_EVENT → on_connect_
            └── 失败 → 清理 stale bond + ble_gap_terminate 看门狗

键盘按键 → ESP_HIDH_INPUT_EVENT
└── HandleBootReport() 解析 8 字节 Boot 报告 → on_key_press_(keycode, modifier)
```

### 7.3 键盘事件 → 应用动作（板文件）

```cpp
// InitializeKeyboard() 中注册（waveshare-s3-rlcd-4.2.cc:292）
bt_keyboard_.OnKeyPress([this](uint8_t keycode, uint8_t modifier) {
    auto& app_ref = Application::GetInstance();
    app_ref.Schedule([this, keycode, modifier]() {   // ⚠️ 跨线程必须 Schedule
        HandleKeyboardKey(keycode, modifier);
    });
});
```

### 7.4 按键映射表（HandleKeyboardKey，板文件）

| HID 键码 | 按键 | 动作 | API |
|---|---|---|---|
| 0x28 | Enter | 开始/停止对话 | `ToggleChatState()` |
| 0x29 | Esc | 停止监听 | `StopListening()` |
| 0x2C | Space | 开始监听 | `StartListening()` |
| 0x52 | ↑ | 音量+10 | `codec->SetOutputVolume(v+10)` |
| 0x51 | ↓ | 音量-10 | `codec->SetOutputVolume(v-10)` |
| 0x13 | R | 截图 | `TakeScreenshot()` |
| 0x15 | T | 录音 | `ToggleRecording()` |
| 0x12 | M | 麦克风静音 | `codec->EnableInput(!enabled)` |
| 0x19 | V | 版本信息 | `ShowVersionPopup()` |
| 0x2B | Tab | 提示音 | `PlaySound(OGG_POPUP)` |
| 0x1E-0x26 | 1-9 | 快捷功能 | 见文档 §5.2 |

---

## 8. 串口命令

| 命令 | 功能 |
|---|---|
| `BTSCAN` | 手动触发 BLE 键盘扫描（10s），自动连接发现的键盘 |

> **无开机自动扫描**：自动扫描连接不可达键盘会泄漏 NimBLE 内存
> （free RAM 117KB→11KB，破坏 OTA 版本检查 + HTTP 服务器）。必须手动触发。

---

## 9. 移植指南（其他板子）

### 9.1 最小移植（5 步）

1. **复制 3 个新文件**：
   - `main/boards/common/bluetooth_keyboard.{h,cc}` → 目标板的 boards/common/
   - `main/workaround_bt_data.lf` → 目标板 main/（若同用 IDF 5.5.2）

2. **Kconfig**：`main/Kconfig.projbuild` 添加 `USE_BLE_HID_KEYBOARD`
   （`depends on IDF_TARGET_*` 改为目标芯片）

3. **CMakeLists**：
   ```cmake
   if (CONFIG_USE_BLE_HID_KEYBOARD)
       list(APPEND SOURCES "boards/common/bluetooth_keyboard.cc")
   endif ()
   # idf_component_register 加:
   #   LINKER_FRAGMENTS workaround_bt_data.lf
   ```

4. **sdkconfig.defaults.<target>**：添加 §4 的 BLE 配置段

5. **板文件集成**（3 处）：
   ```cpp
   #include "bluetooth_keyboard.h"
   BluetoothKeyboard bt_keyboard_;            // 成员
   // 构造函数: InitializeKeyboard();          // 初始化+回调
   // 串口命令: BTSCAN → bt_keyboard_.StartScan(10);
   ```
   按需修改 `HandleKeyboardKey()` 映射表（用目标板的 API）。

### 9.2 必须注意（踩过的坑）

| # | 坑 | 规避 |
|---|---|---|
| 1 | `esp_hidh_dev_open()` **阻塞** | 必须在独立任务调用（`ConnectAsync`），不能在 GAP 回调 |
| 2 | 跨线程访问 Board/Application | 键盘回调必须 `Application::GetInstance().Schedule()` 转发主线程 |
| 3 | 连接失败内存泄漏 | 失败后 `ble_store_util_delete_peer` + `ble_gap_terminate` 看门狗 |
| 4 | 开机自动扫描泄漏 | 只手动 `BTSCAN` 触发 |
| 5 | `BT_NIMBLE_HID_SERVICE` 未设 | 链接错误（nimble_hidh.c 被编译排除） |
| 6 | `sm_mitm=1` + `NO_INPUT_OUTPUT` | 配对必失败，必须 `sm_mitm=0`（Just Works） |
| 7 | 忘了 `MEM_ALLOC_MODE_EXTERNAL` | NimBLE 挤占内部 RAM 导致 AFE 溢出（用 INTERNAL 必踩） |
| 8 | `MEM_ALLOC_MODE_EXTERNAL` 需 workaround | 必须注册 `workaround_bt_data.lf`（见 §6），否则 ldgen 失败 |
| 9 | 键盘需进配对模式 | 广播≠可连接，需键盘按配对键（LED 快闪） |

### 9.3 可移植性说明

- `bluetooth_keyboard.{h,cc}` **与板无关**（只依赖 IDF 公共 API + esp_hid）
- 板相关部分**隔离在** `InitializeKeyboard()` / `HandleKeyboardKey()` / `KeyboardScanNow()`
- 换板只需改：映射表（用目标板 API）+ Kconfig 依赖芯片 + sdkconfig 目标
- 键盘配对记忆在 NVS（`NVS_PERSIST`），换板后首次需重新配对

---

## 10. 验证方法

```powershell
# 1. 串口发 BTSCAN（键盘先进配对模式）
# 2. 观察扫描输出（应显示键盘名称/appearance/HID 服务）
I (2466) ble_keyboard: BLE device: df:3b:27:5e:03:d5  name='MIIIW MECH-KB Pro'  RSSI=-66  appearance=0x03c1  [KEYBOARD]
I (2466) ble_keyboard:   -> advertises HID service (0x1812)
I (2466) ble_keyboard: Keyboard selected: MIIIW MECH-KB Pro

# 3. 连接成功后按 Enter/Esc/方向键等，串口显示
I (xxxx) ble_keyboard: key=0x28       # Enter
I (xxxx) waveshare_rlcd_4_2: BLE key: 0x28

# 4. 内存健康检查（应稳定 >50KB，无泄漏）
python scripts/detect_env.py --health
```

---

## 11. 后续扩展方向

- **文本注入**：新增 `Protocol::SendTextMessage()` + `Application::SendUserText()`，
  需先验证服务器支持 `type:"stt"` 消息
- **多键盘**：`BT_NIMBLE_MAX_CONNECTIONS` > 1 + 设备列表管理
- **连接记忆**：保存键盘地址到 NVS，开机直接连接已配对设备（需防泄漏）
- **屏幕软键盘**：配合触摸屏输入
