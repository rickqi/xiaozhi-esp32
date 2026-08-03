# BLE 蓝牙键盘输入实现计划（MVP 快捷键 + 方向键/数字扩展）

> 基于当前版本 v3.1.0（commit 010c3e4），目标板 waveshare-s3-rlcd-4.2（ESP32-S3）
> 日期：2026-08-03
> 状态：**方案已确认，待实施**
> 版本目标：v3.2.0（minor，新功能）

---

## 0. 方案概述

采用 **esp_hid 组件的 NimBLE HID Host**（官方维护，`esp_hid/src/nimble_hidh.c`），
移植 IDF 官方示例 `examples/bluetooth/esp_hid_host/`，新增 `BluetoothKeyboard` 类
集成到板文件。MVP 阶段实现**键盘快捷键控制**（含方向键音量、数字键快捷功能），
文本注入留作后续扩展。

**技术可行性已确认**：
- ✅ IDF 5.5 官方 `esp_hid` 组件内置 BLE HID Host（NimBLE 后端）
- ✅ `ESP_HIDH_INPUT_EVENT` 事件流完备（`param->input.data` 为原始 HID 报告）
- ✅ 板级集成模式明确（Button 类 std::function 回调 + 构造函数挂钩 + Schedule 跨线程）
- ✅ 内存可行（PSRAM 8MB 已启用，NimBLE 堆配外部模式，仅需 ~15-20KB）
- ✅ 外部参考：breezybox 真实项目 + 官方 esp_hid_host 示例

**硬件限制**：ESP32-S3 仅支持 BLE（无经典蓝牙），老式 BT 2.x/3.x 键盘无法配对。

---

## 1. 键盘映射表（MVP）

### 1.1 已核实的映射 API

| 键盘按键 | HID Usage | 映射动作 | 实际 API | 状态 |
|---|---|---|---|---|
| Enter | 0x28 | 开始/停止对话 | `Application::ToggleChatState()` | ✅ |
| Esc | 0x29 | 停止监听 | `Application::StopListening()` | ✅ |
| Space | 0x2C | 开始监听 | `Application::StartListening()` | ✅ |
| ↑ | 0x52 | 音量 +10 | `codec->SetOutputVolume(v+10)` | ✅ |
| ↓ | 0x51 | 音量 -10 | `codec->SetOutputVolume(v-10)` | ✅ |
| R | 0x13 | 截图 | `board->TakeScreenshot()` | ✅ |
| T | 0x15 | 开始/停止录音 | `board->ToggleRecording()` | ✅ |
| M | 0x12 | 麦克风静音 | `codec->EnableInput(!enabled)` | ✅ |
| V | 0x19 | 版本信息弹窗 | `board->ShowVersionPopup()` | ✅ |
| Tab | 0x2B | 播放提示音 | `app.PlaySound(OGG_POPUP)` | ✅ |

### 1.2 数字键快捷功能（MVP 扩展，预留）

数字键 1-9（HID Usage `0x1E`~`0x26`）：

| 键 | 功能 | API |
|---|---|---|
| 1 | 播放提示音 | `app.PlaySound(OGG_POPUP)` |
| 2 | 重启网络/WiFi | `board` 网络重置 |
| 3 | 系统信息弹窗 | `app.Alert(...)` |
| 4 | 播放最近录音 | `board->PlayLatestRecording()` |
| 5 | 低电量提醒模拟 | `app.Alert(...)` |
| 6 | 启动 HTTP 服务器 | `HttpFileServer::GetInstance().Start(80)` |
| 7 | 停止 HTTP 服务器 | `HttpFileServer::GetInstance().Stop()` |
| 8 | 唤醒词触发模拟 | `app.WakeWordInvoke("你好小智")` |
| 9 | 重启设备 | `app.Reboot()` |

> 所有键码集中在 `HandleKeyboardKey()` 的单个 switch 中，便于增删。

### 1.3 音量调节实现（已核实 `SetOutputVolume`）

```cpp
void AdjustVolume(int delta) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        int v = codec->output_volume() + delta;
        codec->SetOutputVolume(v < 0 ? 0 : (v > 100 ? 100 : v));
    }
}
```
- `AudioCodec::SetOutputVolume(int)` 默认音量 70（audio_codec.h:54），范围 0-100
- 板文件 L2565 已有使用先例

---

## 2. 分步执行计划

### 步骤 1：Kconfig 配置（约 5 分钟）

**1a. `sdkconfig.defaults.esp32s3` 追加**：
```ini
# === BLE HID Host（蓝牙键盘输入）===
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_HID_SERVICE=y
CONFIG_BT_NIMBLE_ROLE_CENTRAL=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
CONFIG_BT_NIMBLE_SM_SC=y
CONFIG_BT_NIMBLE_SM_BONDING=y
CONFIG_BT_NIMBLE_NVS_PERSIST=y
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y
```

**1b. `main/Kconfig.projbuild` 追加**（复用 BluFi select 模式）：
```kconfig
config USE_BLE_HID_KEYBOARD
    bool "Bluetooth HID Keyboard (BLE HID Host)"
    depends on IDF_TARGET_ESP32S3
    default y
    select BT_NIMBLE_ENABLED
    help
        Connect a BLE keyboard as input via esp_hid HID host (MVP: hotkeys).
```

**1c. `main/CMakeLists.txt`**：
```cmake
if(CONFIG_USE_BLE_HID_KEYBOARD)
    list(APPEND SOURCES boards/common/bluetooth_keyboard.cc)
endif()
```

**验收**：`idf.py reconfigure` 无 Kconfig 冲突；`CONFIG_BT_NIMBLE_ENABLED=y` 生效。

### 步骤 2：BluetoothKeyboard 类（约 1-2 小时）

**2a. 新增 `main/boards/common/bluetooth_keyboard.h`**：
- 类结构仿 Button 模式：`std::function` 回调（OnKeyPress/OnConnect/OnDisconnect）
- 方法：`Init()` / `StartScan()` / `Connect()` / `Disconnect()`

**2b. 新增 `main/boards/common/bluetooth_keyboard.cc`**（移植官方示例）：
- `Init()`：`esp_hidh_init()` → BT 控制器初始化（`esp_bt_controller_get_status()` 检查避免与 BluFi 冲突）→ `esp_nimble_enable()`
- 事件处理：OPEN → on_connect；INPUT（usage=KEYBOARD）→ 解析报告 → on_key_press；CLOSE → `esp_hidh_dev_free()` → on_disconnect
- 扫描：`ble_gap_disc()` + `ble_hs_adv_parse_fields()` 过滤 appearance 0x03C1
- HID 报告解析：Boot 报告 8 字节（data[0]=modifier, data[2..7]=keycodes）

**验收**：编译通过；串口日志显示 NimBLE host 启动、scan 发起。

### 步骤 3：板文件集成（约 30 分钟）

**3a. `waveshare-s3-rlcd-4.2.cc`**：
- include + 成员 `BluetoothKeyboard bt_keyboard_;`
- 构造函数 `InitializeButtons()` 后调 `InitializeKeyboard()`
- `InitializeKeyboard()`：Init + OnKeyPress（经 `app.Schedule()` 跨线程转发）+ OnConnect（屏幕通知"键盘已连接"）+ StartScan

**3b. `HandleKeyboardKey()`**：按 §1 映射表实现 switch

**验收**：构建成功；烧录后键盘配对 → Enter 触发对话切换。

### 步骤 4：数字键快捷功能（约 30 分钟）

按 §1.2 表实现 `case 0x1E: ... case 0x26:` 分组。涉及新依赖：`HttpFileServer`、`WakeWordInvoke`、`PlayLatestRecording`。

**验收**：数字键功能逐项测试。

### 步骤 5：构建 + 烧录 + 验证（约 1 小时）

1. `python scripts/detect_env.py --health`（环境就绪确认）
2. `idf.py build`（首次启用 BT 编译需几分钟）
3. `idf.py -p $env:XIAOZHI_PORT flash`
4. 键盘配对（键盘进配对模式 → 自动连接，Just Works）
5. 验证：串口日志 `OPEN...` + `INPUT: KEYBOARD`；逐键测试；`SELFTEST` 无回归

### 步骤 6：文档 + 版本（约 20 分钟）

1. `CMakeLists.txt`：`PROJECT_VER "3.2.0"`
2. `CHANGELOG.md`：v3.2.0 条目（feat: BLE 键盘输入）
3. `main/version_info.cc` `kFeatures[]`：+蓝牙键盘输入
4. `docs/usage.md` + 板 README：键盘映射表 + 配对说明
5. 提交 + 推送

---

## 3. 风险清单

| # | 风险 | 应对 |
|---|---|---|
| 1 | `BT_NIMBLE_HID_SERVICE` 未生效 → 链接错误 | 步骤 1 验收确认 |
| 2 | 键盘默认 Boot 模式 → 通知不订阅 | 写 Protocol Mode(0x2A4E)=1 强制 Report 模式 |
| 3 | 配对无 UI | Just Works（sm_mitm=0）+ NVS 持久化 |
| 4 | 内存紧张 | NimBLE 堆 PSRAM 外部模式；--health 监测 |
| 5 | 跨线程 | 键盘回调经 `app.Schedule()` 转发主线程 |
| 6 | BLE+WiFi 共存 | ESP32-S3 双模共用天线，测试注意干扰 |

---

## 4. 后续扩展（非 MVP）

- **文本注入**：新增 `Protocol::SendTextMessage()` + `Application::SendUserText()`；需先验证服务器支持 `type:"stt"` 消息
- 键盘连接列表选择 / 记住多设备
- 屏幕软键盘（配合触摸板）

---

## 5. 参考资料

- IDF 官方示例：`D:\esp\esp-idf\examples\bluetooth\esp_hid_host\`
- HID Host 实现：`D:\esp\esp-idf\components\esp_hid\src\nimble_hidh.c`（966 行）
- HID Host API：`D:\esp\esp-idf\components\esp_hid\include\esp_hidh.h`
- 外部参考：breezybox（`valdanylchuk/breezybox` `src/components/breezy_bt/bt_keyboard.c`）
- 板级集成参考：`main/boards/common/button.{h,cc}` + `blufi.cpp`（NimBLE 初始化序列）
