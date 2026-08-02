# 通知显示机制分析报告

> 分析对象：waveshare-s3-rlcd-4.2 板（ESP32-S3，master 分支）
> 分析范围：UI 通知布局、启动/自检冲突、FreeRTOS 多核多任务模型
> 分析日期：2026-08-02

---

## 1. 通知布局

### 1.1 位置（默认变体）

当前配置 `CONFIG_USE_WECHAT_MESSAGE_STYLE` **未启用**（sdkconfig:728），使用默认（非微信）变体。

`notification_label_` 创建于 `lcd_display.cc:934-942`：

```
┌──────────────────────────────────────┐
│ 🔔 状态栏(顶部) / 循环显示             │  ← status_bar_ / status_label_ / status_icon_row_
│                                      │
│                                      │
│              [通知文本]               │  ← notification_label_（底部横幅）
│          (底部上方 70px 居中)          │    LV_ALIGN_BOTTOM_MID, 0, -70
│──────────────────────────────────────│
│        聊天消息区 (bottom_bar_)        │  ← chat_message_label_
└──────────────────────────────────────┘
```

| 属性 | 值 |
|---|---|
| 父对象 | `screen`（顶层独立覆盖层） |
| 位置 | 底部上方 **70px** 居中（`LV_ALIGN_BOTTOM_MID, 0, -70`，lcd_display.cc:941） |
| 宽度 | 全屏宽 - spacing(8) |
| 换行 | `LV_LABEL_LONG_WRAP` 自动换行 |
| 对齐 | 居中（`LV_TEXT_ALIGN_CENTER`） |
| 初始状态 | **HIDDEN**（创建即隐藏，lcd_display.cc:942） |

> 微信风格变体（`CONFIG_USE_WECHAT_MESSAGE_STYLE=y`）将通知置于顶部状态栏居中（lcd_display.cc:441-447），本板未启用。

### 1.2 样式
- 纯文本：无背景、无边框、无圆角、无动画
- 颜色：主题 `text_color`（`SetTheme` 时同步更新，lcd_display.cc:1256）
- 字体：继承屏幕 `text_font`

### 1.3 功能流程

**显示** `LvglDisplay::ShowNotification(text, duration_ms=3000)`（lvgl_display.cc:89-100）：
1. 设置文本
2. 显示通知、**隐藏 status_label_**（lvgl_display.cc:96）
3. 停止旧定时器并重启一次性 `notification_timer_`（duration_ms 后隐藏）

**消失**：
- 正常：定时器回调只隐藏通知（lvgl_display.cc:26），不恢复 status_label_（由循环显示/SetStatus 负责）
- 抢占：任何 `SetStatus()` 立即隐藏通知（lvgl_display.cc:80）
- 替换：连续调用自动重置定时器，无叠加

### 1.4 触发源（本板）

通过 `ShowNotify(msg, duration_ms)` 包装（waveshare-s3-rlcd-4.2.cc:674-680）：

| 类别 | 消息 | 时长 |
|---|---|---|
| 录音 | 开始/进度/完成/失败 | 3000-5000ms |
| 播放 | 进度/完成/失败 | 3000ms |
| 音乐 | 歌名/参数/进度 | 8000ms（歌名）/4000ms/5000ms |
| 自检 | 结果/忙 | 5000ms/2000ms |
| 系统信息 | IP/MAC/版本 | 6000ms |
| 截图 | 尺寸信息 | 4000ms |

**全局**：网络事件（30s，application.cc:119/131/138）、音量/静音（3s，各板）、AEC 模式切换等。

---

## 2. 启动时序与自检冲突分析

### 2.1 启动时序（电 → 正常运行）

| 阶段 | 内容 | 对通知的影响 |
|---|---|---|
| 1. 板构造 | 构造函数：I2C/RTC/按键/工具/显示/ADC/SD（waveshare...cc:1847-1860） | 通知标签创建（隐藏） |
| 2. 初始化 | `Application::Initialize()`：时钟定时器(1s)、网络事件回调注册（application.cc:61-176） | 启动网络事件 → `ShowNotification`(30s) |
| 3. 网络连接 | `Scanning`→`Connecting`→`Connected` 各调 `ShowNotification(30000)` | 占用底部通知区，最长 30s |
| 4. 激活任务 | `SetStatus(CHECKING...)` / `SetStatus(LOADING_PROTOCOL)`（application.cc:419/491） | 切到顶部状态栏，隐藏通知 |
| 5. 激活完成 | `ShowNotification("版本 x.x.x")`(3s)（application.cc:324）→ 随后 `SetStatus(STANDBY)` | ⚠️ 竞争窗口 |
| 6. 进入空闲 | `SetStatus(STANDBY)` + 空闲时钟逻辑 | 状态栏常驻 |

### 2.2 自检（SELFTEST）触发机制

**自检不在启动时运行。** 只有两个手动入口：
1. 串口命令 `SELFTEST`（waveshare...cc:1531-1533，`ScreenshotCmdTask` 监听）
2. MCP 工具 `self.run_self_test`（waveshare...cc:1368-1380）

构造函数**从不调用** `RunSelfTest`（已全量 grep 验证）。

自检内容（7 项）：Display（直接写 framebuffer）、Buttons、SDCard、Battery、RTC、SHTC3、AudioEcho。

### 2.3 冲突结论

| 项目 | 结论 |
|---|---|
| 启动时自检↔通知冲突 | ✅ **无**（自检仅手动触发） |
| 启动时唯一写通知的 | `AutoScreenshotTask` +12s 截图通知（waveshare...cc:1511，4s），良性 |
| 自检触发时屏闪 | ⚠️ 显示自检项直接写 framebuffer（`RLCD_ColorClear(白)`×2 + 角像素），绕过 LVGL，整屏短暂白闪（有 `DisplayLockGuard` 防竞态） |
| 自检结果占用通知区 | ⚠️ `ShowNotify("SelfTest: 7/7 ALL PASS", 5000)` 写**同一个底部通知标签**，占用 5s，顶掉其他通知（last-writer-wins 设计） |
| 状态循环行（温度/湿度/日期/时钟/电池） | ✅ 只写**顶部** `status_icon_row_`（lcd_display.cc:1053），从不触碰底部通知，空间隔离 |
| `SetTheme`（含 MCP `self.screen.set_theme` 运行时调用） | ✅ 只改颜色/字体，不重定位/隐藏/重建标签（lcd_display.cc:1212-1352） |
| `SetupUI` 重建 | ✅ 仅在构造函数跑一次，运行期不重建屏幕 |
| 屏幕清屏 | ✅ 全代码无 `lv_obj_clean`/清屏逻辑 |

### 2.4 正常运行时的三个"隐藏通知"机制

| # | 机制 | 位置 | 触发条件 | 频率 |
|---|---|---|---|---|
| **C1** | **空闲时钟抢占** | `lvgl_display.cc:125-134` → `:80` | 设备 Idle 且距上次 `SetStatus` >10s，每秒 tick 调 `SetStatus(时钟)` | **最激进：空闲后每 1s 一次** |
| C2 | 定时器到期 | `lvgl_display.cc:26`（回调） | `ShowNotification` 后 duration_ms 到 | 正常设计 |
| C3 | 状态切换 | `application.cc:848/854/859/877` → `:80` | Idle/Connecting/Listening/Speaking 切换 | 每次状态变化 |

**C1 是实际影响"通知默认可见性"的最大问题**（**已于 2026-08-02 修复**，见 §2.6）：
- 原缺陷：`ShowNotification` **不重置** `last_status_update_time_`（lvgl_display.cc:44/82）
- 原缺陷：设备空闲 >10s 后，任何短时通知（3s 音量提示、音乐歌名）会在下一秒被时钟刷新隐藏
- 原缺陷：网络事件 30s 通知因发生在启动/非空闲期，才得以完整显示

**次要问题**（未修复）：启动完成时 `ShowNotification("版本...")`（application.cc:324）被 `SetStatus(STANDBY)`（application.cc:848）竞争隐藏——按主循环事件位顺序（ACTIVATION_DONE 先于 STATE_CHANGED），版本通知基本看不见。

### 2.5 结论

1. 底部通知**布局正确**、位置固定，与聊天区无重叠
2. 启动自检**不会**与通知冲突（自检仅手动触发）
3. "底部通知是正常运行默认显示"**不成立**——通知是瞬态横幅，默认隐藏
4. **C1 空闲时钟抢占（唯一实质性显示缺陷）已修复**（2026-08-02），短通知与长通知现均可完整显示满 duration_ms

### 2.6 C1 修复记录（2026-08-02）

**修复目标**：空闲时钟抢占不再秒杀短通知（3s）或抢占长通知（30s 网络事件）。

**改动文件**：`main/display/lvgl_display/lvgl_display.cc`（两处）

**改动 1** —— `ShowNotification()`（原 :89-100）新增一行：
```cpp
    lv_label_set_text(notification_label_, notification);
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    // Reset the idle-clock window so the notification is not immediately
    // preempted by the clock SetStatus in UpdateStatusBar (C1 fix).
    last_status_update_time_ = std::chrono::system_clock::now();

    esp_timer_stop(notification_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_, duration_ms * 1000));
```
效果：每次显示通知时重置 10s 空闲时钟窗口 → 短通知（duration_ms ≤ 10s）可完整显示。

**改动 2** —— `UpdateStatusBar()` 空闲时钟分支（原 :125-139）：
```cpp
    if (app.GetDeviceState() == kDeviceStateIdle) {
        if (last_status_update_time_ + std::chrono::seconds(10) < std::chrono::system_clock::now()) {
            bool notification_visible = false;
            {
                DisplayLockGuard lock(this);
                notification_visible = (notification_label_ != nullptr) &&
                    !lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
            }
            // Skip the clock preemption while a notification is on screen
            // (C1 fix), so long-duration notifications (e.g. 30s network
            // events) are not hidden by the idle clock.
            if (!notification_visible) {
                // Set status to clock "HH:MM"
                ...
            }
        }
    }
```
效果：通知可见期间，空闲时钟跳过 `SetStatus(时钟)`，长通知（>10s）不再被抢占；通知隐藏后的下一个 tick 立即恢复时钟显示。

**设计考量**：
- 通知可见性检查在 `DisplayLockGuard` 内进行，与 LVGL 互斥锁串行化模型一致（§3.3）
- 通知隐藏后（定时器回调），`last_status_update_time_` 已过期 → 下一 1s tick 立即恢复时钟，无状态残留
- C2（定时器到期）、C3（状态切换 SetStatus）保持原行为不变

**验证状态**：编译验证通过（见 §5）。

### 2.7 待办建议（未实施）

1. **修复启动版本通知竞争**：application.cc:324 与 :848 的时序
2. **自检屏闪**（可选）：Display 自检项跳过非 Idle 状态（镜像 Audio 检查的守卫 waveshare...cc:1737-1741），或将自检限制在诊断模式
3. **运行时核证明**（可选）：在 ShowNotification/定时器回调/lvgl_port_task 中加 `esp_cpu_get_core_id()` 日志，实证各任务实际落核
4. **TakeScreenshot 无锁读像素**（waveshare...cc:1470-1481）：当前安全仅因单色显示器像素在自有 PSRAM LUT；换 RGB LCD 需加锁

---

## 3. FreeRTOS 多核多任务模型分析

### 3.1 核心配置（决定性）

**双核 SMP 已启用。**

- `sdkconfig:1768` → `# CONFIG_FREERTOS_UNICORE is not set`（双核）
- `sdkconfig:1769` → `CONFIG_FREERTOS_HZ=100`
- `sdkconfig:1596-1597` → WDT 同时检查 CPU0 和 CPU1 空闲任务（证明双核运行）
- `sdkconfig.defaults.esp32s3` 无 UNICORE 条目 → 默认双核
- `main/` 全代码无 `esp_cpu_get_core_id`/`xPortGetCoreID` 调用 → 代码从不自查核心，完全依赖互斥锁

### 3.2 任务清单（显示相关）

| 任务名 | 创建位置 | 栈 | 优先级 | 核心 | 说明 |
|---|---|---|---|---|---|
| **taskLVGL**（LVGL 渲染） | `esp_lvgl_port.c:84-86` | 7168 | **2**（本板覆盖 custom_lcd_display.cc:98；通用 LcdDisplay 为 1 且绑定 core1） | **-1 = 任意核**（本板未覆盖；通用 LcdDisplay 绑 core1） | 持有 lvgl_mux 执行 `lv_task_handler` |
| **esp_timer 任务**（系统） | IDF 启动时创建 | — | ~22（ESP_TIMER_TASK_PRIORITY） | 任意核 | 执行所有 `ESP_TIMER_TASK` 回调，含通知隐藏回调 |
| **main 任务**（ESP-IDF） | IDF 启动时创建 | — | 1 | 任意核 | 运行 `Application::Run()` 无限循环，绝大多数 SetStatus/ShowNotification 在此执行 |
| **audio_input** | `audio_service.cc:84-88` | 6144 | **8** | **0（绑定）** | 最高优先级音频任务 |
| **audio_output** | `audio_service.cc:91-95` | 4096 | 4 | 任意核 | |
| **opus_codec** | `audio_service.cc:113-117` | 26624 | 2 | 任意核 | |
| **activation** | `application.cc:283-288` | 8192 | 2 | 任意核 | 激活流程，只置事件位不直接调显示 |
| **scr_cmd**（ScreenshotCmdTask） | `waveshare...cc:1855` | 6144 | 1 | **1（绑定）** | 串口命令监听，调 TakeScreenshot→ShowNotification |
| **scr_auto**（AutoScreenshotTask） | `waveshare...cc:1856` | 6144 | 1 | **1（绑定）** | 开机 +12s 一次性截图 |

### 3.3 锁架构：单一全局递归互斥锁

**DisplayLockGuard**（display.h:58-71）：RAII 守卫，构造时 `Lock(30000)`（30s 超时），析构时 `Unlock()`。

**锁不在应用代码里**，各子类委托给 esp_lvgl_port 全局递归互斥锁：
- `LcdDisplay::Lock()` → `lvgl_port_lock()`（lcd_display.cc:353-355）
- 本板 `CustomLcdDisplay : public LcdDisplay`（custom_lcd_display.h:20）继承该实现

**互斥锁本体**（esp_lvgl_port.c:75）：`xSemaphoreCreateRecursiveMutex()` —— **FreeRTOS 递归互斥锁**（`SemaphoreHandle_t`），**不是** `portMUX_TYPE`（ISR 自旋锁，未使用）。

`lvgl_port_lock`（esp_lvgl_port.c:137-143）：`xSemaphoreTakeRecursive(lvgl_mux, timeout)`。

**可持锁的任务**：任何构造 DisplayLockGuard 的任务——LVGL 渲染任务、esp_timer 任务、main 任务、scr_cmd/scr_auto 任务。因递归性质，已持锁任务可重入不产生死锁。

### 3.4 通知显示调用上下文（哪个任务在调）

| 调用点 | 位置 | 实际执行任务 |
|---|---|---|
| 启动网络事件通知 | application.cc:119-491 | main 任务 |
| 激活完成版本通知 | application.cc:324 | main 任务（HandleActivationDoneEvent 在 Run 循环中） |
| 对话状态 SetStatus | application.cc:692-877 | main 任务 |
| 全部 `Schedule([...])` lambda（MCP/协议回调） | application.cc:896-902 | **汇流到 main 任务**（推入 main_tasks_ + 置 MAIN_EVENT_SCHEDULE 位 → Run 循环 :249-256 消费）。MCP 工具/协议 JSON 处理器**不是独立任务** |
| 板按键音量通知 | waveshare...cc:225 | main 任务（经 Schedule） |
| 音乐播放通知 | waveshare...cc:662/671 | 串口触发→scr_cmd(core1)；MCP 触发→main 任务 |
| 截图通知 | waveshare...cc:1511 | scr_cmd/scr_auto（core1） |
| **通知隐藏回调** | lvgl_display.cc:21-27 | **esp_timer 任务** |
| UpdateStatusBar（时钟/电池/网络图标） | lvgl_display.cc:102-204 ← application.cc:261 | main 任务（CLOCK_TICK 处理器） |

### 3.5 esp_timer 分发机制

**创建**（lvgl_display.cc:18-33）：`dispatch_method = ESP_TIMER_TASK`（lvgl_display.cc:29）。

**`ESP_TIMER_TASK` 语义**：回调**不在** ISR 上下文执行，而是投递到**单一专用 `esp_timer` FreeRTOS 任务**（系统启动时创建，优先级 ~22 非常高，**不绑定核心**，可迁移）。该任务**串行执行系统内所有 `ESP_TIMER_TASK` 回调**——任意两个 esp_timer 回调不会并发。

因此通知隐藏回调在高优先级、不绑核、单实例任务上运行，通过 DisplayLockGuard 取 lvgl_mux 后再隐藏 `notification_label_`。若 LVGL 渲染任务正持锁，esp_timer 任务最多阻塞 30s。

### 3.6 结论：是否采用 FreeRTOS 多核多任务？

**是 —— 真多任务 + 多核（SMP 启用）**，但 **LVGL/显示对象访问被单一全局递归互斥锁完全串行化**：

- **触碰通知/状态标签的任务**：taskLVGL（任意核）、esp_timer 任务（任意核）、main 任务（任意核）、scr_cmd/scr_auto（core1）
- **核心分布**：taskLVGL 与 main 任务**不绑核**（调度器自由放置），scr_cmd/scr_auto 绑 **core1**，audio_input 绑 **core0** → 显示相关代码**确实可同时在两个核上执行**
- **并行度判定**：跨核/跨线程**并发执行**存在（多核多任务特征），但**通知/LVGL 对象上零并行**——每个对 `notification_label_`/`status_label_` 的读写都包裹在 DisplayLockGuard → lvgl_mux 递归互斥锁中，强制互斥

**设计本质：多线程/多核能力 + 互斥锁串行化**（非无锁并行）。

通知生命周期：
```
写者任务(main/scr_cmd等) --取锁--> 设置文本+显示+启动esp_timer --放锁
                                          ↓ duration_ms 后
esp_timer任务 --取同一把锁--> 隐藏通知 --放锁
```
两端通过同一把锁串行化，可能落在不同核心，但绝不并发。

---

## 4. 待办建议（如需要）

> ✅ **C1 空闲时钟抢占已于 2026-08-02 修复**（见 §2.6），从本清单移除。

1. **修复启动版本通知竞争**：application.cc:324 与 :848 的时序
2. **自检屏闪**（可选）：Display 自检项跳过非 Idle 状态（镜像 Audio 检查的守卫 waveshare...cc:1737-1741），或将自检限制在诊断模式
3. **运行时核证明**（可选）：在 ShowNotification/定时器回调/lvgl_port_task 中加 `esp_cpu_get_core_id()` 日志，实证各任务实际落核
4. **TakeScreenshot 无锁读像素**（waveshare...cc:1470-1481）：当前安全仅因单色显示器像素在自有 PSRAM LUT；换 RGB LCD 需加锁

---

## 5. 变更记录

| 日期 | 变更 | 验证 |
|---|---|---|
| 2026-08-02 | C1 修复：`lvgl_display.cc` 两处改动（ShowNotification 重置空闲时钟窗口；空闲时钟跳过可见通知） | 编译通过，无警告无错误 |

---

## 附录：参考文件与关键行号

| 文件 | 关键行 |
|---|---|
| `main/display/lcd_display.cc` | 通知创建 934-942；状态循环 1053-1141；Lock 353-355；SetTheme 1212-1352 |
| `main/display/lvgl_display/lvgl_display.cc` | 定时器 18-33；SetStatus 73-83；ShowNotification 89-100；空闲时钟 125-134 |
| `main/display/lvgl_display/lvgl_display.h` | notification_timer_ :45；last_status_update_time_ :44 |
| `main/display/display.h` | DisplayLockGuard 58-71 |
| `main/application.cc` | 网络通知 119/131/138；激活完成 309-334；状态变更 836-894；Schedule 896-902；激活任务 283-288 |
| `main/boards/waveshare-s3-rlcd-4.2/waveshare-s3-rlcd-4.2.cc` | ShowNotify 674-680；自检 1368-1844；构造函数 1847-1860；截图 1456-1593 |
| `main/boards/waveshare-s3-rlcd-4.2/custom_lcd_display.cc` | LVGL 端口配置 97-100 |
| `managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port.c` | lvgl_mux 创建 :75；任务创建 83-87；lvgl_port_lock 137-143 |
| `managed_components/espressif__esp_lvgl_port/include/esp_lvgl_port.h` | 配置宏默认值 64-71 |
| `sdkconfig` | UNICORE 未设 :1768；HZ=100 :1769 |
