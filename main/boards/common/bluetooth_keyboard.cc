#include "bluetooth_keyboard.h"

#include <cstring>

#include "esp_bt.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_hs_adv.h"
#include "services/gap/ble_svc_gap.h"
#endif

#define TAG "ble_keyboard"

// BLE HID appearance value for keyboards (Bluetooth SIG assigned)
#define ESP_HID_APPEARANCE_KEYBOARD 0x03C1

// Boot keyboard report layout
#define HID_KBD_MOD_LSHIFT 0x02
#define HID_KBD_REPORT_LEN 8

// Maximum concurrent scan result records we buffer
#define MAX_SCAN_RESULTS 8

static BluetoothKeyboard* s_instance = nullptr;

BluetoothKeyboard::BluetoothKeyboard() {
    s_instance = this;
}

BluetoothKeyboard::~BluetoothKeyboard() {
    if (s_instance == this) {
        s_instance = nullptr;
    }
    Disconnect();
}

// ---------------------------------------------------------------------------
// BT controller / NimBLE host init (ported from blufi.cpp NimBLE path)
// ---------------------------------------------------------------------------

esp_err_t BluetoothKeyboard::ControllerInit() {
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
#ifdef CONFIG_BT_NIMBLE_ENABLED
    ret = esp_nimble_init();
    if (ret) {
        ESP_LOGE(TAG, "esp_nimble_init() failed: %s", esp_err_to_name(ret));
        return ret;
    }
#endif
    return ESP_OK;
}

esp_err_t BluetoothKeyboard::HostInit() {
#ifdef CONFIG_BT_NIMBLE_ENABLED
    ble_hs_cfg.sync_cb = [](void) {
        ESP_LOGI(TAG, "NimBLE host synced");
    };
    ble_hs_cfg.reset_cb = [](int reason) {
        ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
    };
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // Just Works pairing: no display/input on device
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;   // MUST be 0 for NO_INPUT_OUTPUT
    ble_hs_cfg.sm_sc = 1;     // LE Secure Connections

    esp_err_t ret = esp_nimble_enable((void*)NimbleHostTask);
    if (ret) {
        ESP_LOGE(TAG, "esp_nimble_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
#endif
    return ESP_OK;
}

void BluetoothKeyboard::NimbleHostTask(void* param) {
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();  // blocks until nimble_port_stop()
    nimble_port_freertos_deinit();
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void BluetoothKeyboard::Init() {
    // If BT controller already initialized (e.g. by BluFi), skip controller init
    esp_bt_controller_status_t status = esp_bt_controller_get_status();
    if (status == ESP_BT_CONTROLLER_STATUS_IDLE) {
        ESP_LOGI(TAG, "Initializing BT controller (BLE)");
        if (ControllerInit() != ESP_OK) {
            return;
        }
    } else {
        ESP_LOGI(TAG, "BT controller already initialized (status=%d), reusing", status);
    }
    bt_initialized_ = true;

    esp_hidh_config_t config = {};
    config.callback = EventHandler;
    config.event_stack_size = 4096;
    config.callback_arg = nullptr;
    if (esp_hidh_init(&config) != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidh_init failed");
        return;
    }

    if (HostInit() != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE host init failed");
        return;
    }
    ESP_LOGI(TAG, "BLE HID host ready");
}

void BluetoothKeyboard::StartScan(uint32_t seconds) {
#ifdef CONFIG_BT_NIMBLE_ENABLED
    struct ble_gap_disc_params disc_params = {};
    disc_params.filter_duplicates = 1;
    disc_params.passive = 0;
    disc_params.itvl = 0x50;
    disc_params.window = 0x30;
    disc_params.filter_policy = 0;

    // Store scan context on heap; freed in gap callback when complete.
    auto* ctx = new BleScanCtx{this, seconds};
    int rc = ble_gap_disc(0, seconds * 1000, &disc_params, GapEventCallback, ctx);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        delete ctx;
    } else {
        ESP_LOGI(TAG, "Scanning for BLE keyboards (%us)...", seconds);
    }
#else
    ESP_LOGE(TAG, "NimBLE not enabled");
#endif
}

void BluetoothKeyboard::Connect(const uint8_t* bda, uint8_t addr_type) {
    if (dev_) {
        ESP_LOGW(TAG, "Already connected; disconnect first");
        return;
    }
    // esp_hidh_dev_open is blocking for NimBLE (performs full discovery)
    ESP_LOGI(TAG, "Opening HID device %02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    esp_hidh_dev_t* dev = esp_hidh_dev_open(
        (uint8_t*)bda, ESP_HID_TRANSPORT_BLE, addr_type);
    if (dev) {
        dev_ = dev;
        ESP_LOGI(TAG, "HID device opened: %s", esp_hidh_dev_name_get(dev));
    } else {
        ESP_LOGE(TAG, "esp_hidh_dev_open returned NULL");
        // Connection failed. Delete the stale NimBLE bond/peer record so it
        // does not accumulate across reboots and leak memory (observed:
        // repeated attempts to a reachable-but-not-pairing keyboard dropped
        // free heap to ~13KB and broke OTA version check / HTTP server).
#ifdef CONFIG_BT_NIMBLE_ENABLED
        ble_addr_t peer_addr;
        memcpy(peer_addr.val, bda, 6);
        peer_addr.type = addr_type;
        int rc = ble_store_util_delete_peer(&peer_addr);
        if (rc == 0) {
            ESP_LOGI(TAG, "Cleared stale NimBLE bond for %02x:%02x:%02x:%02x:%02x:%02x",
                     bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        }
#endif
    }
}

void BluetoothKeyboard::Disconnect() {
    if (dev_) {
        esp_hidh_dev_close(dev_);
        // esp_hidh_dev_free must be called on CLOSE_EVENT; but on explicit
        // disconnect we free immediately to avoid dangling pointer.
        esp_hidh_dev_free(dev_);
        dev_ = nullptr;
        if (on_disconnect_) {
            on_disconnect_();
        }
    }
}

// ---------------------------------------------------------------------------
// esp_hidh event callback (runs on esp_hidh_events task)
// ---------------------------------------------------------------------------

void BluetoothKeyboard::EventHandler(void* handler_args, esp_event_base_t base,
                                     int32_t id, void* event_data) {
    BluetoothKeyboard* self = s_instance;
    if (!self) {
        return;
    }
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t* param = (esp_hidh_event_data_t*)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT: {
        esp_hidh_dev_t* dev = param->open.dev;
        if (param->open.status == ESP_OK) {
            ESP_LOGI(TAG, "OPEN: %s", esp_hidh_dev_name_get(dev));
            self->dev_ = dev;
            if (self->on_connect_) {
                self->on_connect_();
            }
        } else {
            ESP_LOGE(TAG, "OPEN failed: %d", param->open.status);
        }
        break;
    }
    case ESP_HIDH_INPUT_EVENT: {
        if (param->input.usage == ESP_HID_USAGE_KEYBOARD) {
            self->HandleBootReport(param->input.data, param->input.length, 0);
        }
        break;
    }
    case ESP_HIDH_CLOSE_EVENT: {
        ESP_LOGI(TAG, "CLOSE: reason=%d", param->close.reason);
        esp_hidh_dev_t* dev = param->close.dev;
        if (self->dev_ == dev) {
            self->dev_ = nullptr;
        }
        esp_hidh_dev_free(dev);  // MUST free in CLOSE handler
        if (self->on_disconnect_) {
            self->on_disconnect_();
        }
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// HID report parsing (Boot keyboard report, 8 bytes)
// ---------------------------------------------------------------------------

void BluetoothKeyboard::HandleBootReport(const uint8_t* data, uint16_t len,
                                         uint8_t /*modifier_override*/) {
    if (len < HID_KBD_REPORT_LEN) {
        return;
    }
    if (!on_key_press_) {
        return;
    }
    uint8_t modifier = data[0];
    bool shift = (modifier & HID_KBD_MOD_LSHIFT) != 0;
    for (int i = 2; i < HID_KBD_REPORT_LEN && data[i] != 0; i++) {
        on_key_press_(data[i], modifier);
        // Also emit ASCII for printable keys (for future text-input use).
        char ch = KeycodeToAscii(data[i], shift);
        if (ch) {
            ESP_LOGI(TAG, "key=0x%02x '%c'", data[i], ch);
        } else {
            ESP_LOGI(TAG, "key=0x%02x", data[i]);
        }
    }
}

char BluetoothKeyboard::KeycodeToAscii(uint8_t keycode, bool shift) {
    // USB HID Usage Table 1.12 — Keyboard/Keypad page (0x07)
    if (keycode >= 0x04 && keycode <= 0x1D) {  // a-z
        char base = 'a' + (keycode - 0x04);
        return shift ? (char)(base - 32) : base;
    }
    if (keycode >= 0x1E && keycode <= 0x27) {  // 1-9, 0
        static const char digits[] = "1234567890";
        static const char shifted[] = "!@#$%^&*()";
        int idx = keycode - 0x1E;
        return shift ? shifted[idx] : digits[idx];
    }
    switch (keycode) {
    case 0x28: return '\n';  // Enter
    case 0x2C: return ' ';   // Space
    case 0x2D: return '-';
    case 0x2E: return '=';   // or '=' shifted '+'
    case 0x2F: return '[';
    case 0x30: return ']';
    case 0x33: return ';';
    case 0x34: return '\'';
    case 0x36: return ',';
    case 0x37: return '.';
    case 0x38: return '/';
    case 0x35: return '`';
    case 0x2B: return '\t';  // Tab
    default: return 0;
    }
}

// ---------------------------------------------------------------------------
// NimBLE GAP scan callback (finds keyboards by appearance 0x03C1)
// ---------------------------------------------------------------------------

int BluetoothKeyboard::GapEventCallback(struct ble_gap_event* event, void* arg) {
    auto* ctx = static_cast<BleScanCtx*>(arg);
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
        if (rc != 0) {
            return 0;
        }
        if (fields.appearance_is_present &&
            fields.appearance == ESP_HID_APPEARANCE_KEYBOARD) {
            ESP_LOGI(TAG, "Keyboard found: %02x:%02x:%02x:%02x:%02x:%02x (RSSI %d)",
                     event->disc.addr.val[0], event->disc.addr.val[1],
                     event->disc.addr.val[2], event->disc.addr.val[3],
                     event->disc.addr.val[4], event->disc.addr.val[5],
                     event->disc.rssi);
            if (ctx->keyboard_addr_found == 0) {
                memcpy(ctx->keyboard_addr, event->disc.addr.val, 6);
                ctx->keyboard_addr_type = event->disc.addr.type;
                ctx->keyboard_addr_found = 1;
            }
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE: {
        if (ctx->keyboard_addr_found) {
            ESP_LOGI(TAG, "Scan complete, connecting to keyboard...");
            if (ctx->self) {
                // IMPORTANT: esp_hidh_dev_open() is BLOCKING (performs full GATT
                // discovery + MTU exchange). Calling it from this GAP callback
                // would stall the NimBLE host task and starve the audio/AFE
                // tasks (observed as "AFE ringbuffer full" floods). Spawn a
                // dedicated task instead.
                ctx->self->ConnectAsync(ctx->keyboard_addr, ctx->keyboard_addr_type);
            }
        } else {
            ESP_LOGI(TAG, "Scan complete, no keyboard found (reason=%d)",
                     event->disc_complete.reason);
        }
        delete ctx;
        return 0;
    }
    default:
        return 0;
    }
}

// Runs on a dedicated task so the blocking esp_hidh_dev_open() never stalls
// the NimBLE host task / audio tasks.
void BluetoothKeyboard::ConnectAsync(const uint8_t* bda, uint8_t addr_type) {
    auto* addr = new uint8_t[6];
    memcpy(addr, bda, 6);
    auto* task = new ConnectTaskArgs{this, addr, addr_type};
    xTaskCreate(ConnectTask, "kbd_connect", 6 * 1024, task, 3, nullptr);
}

void BluetoothKeyboard::ConnectTask(void* arg) {
    auto* args = static_cast<ConnectTaskArgs*>(arg);
    BluetoothKeyboard* self = args->self;
    if (self) {
        // esp_hidh_dev_open() blocks for up to ~30s (ble_gap_connect timeout).
        // If the keyboard is not in pairing mode, it hangs until then and the
        // NimBLE connection context is retained, leaking memory. Watchdog:
        // after the call returns (success or fail), force-terminate any stale
        // NimBLE connection so its context is freed.
        self->Connect(args->bda, args->addr_type);
        // Give the esp_hidh event loop a moment to settle, then clean up.
        vTaskDelay(pdMS_TO_TICKS(200));
#ifdef CONFIG_BT_NIMBLE_ENABLED
        // 0x13 = Remote User Terminated Connection (HCI error code)
        int rc = ble_gap_terminate(BLE_HS_CONN_HANDLE_NONE, 0x13);
        if (rc != 0 && rc != BLE_HS_ENOTCONN) {
            ESP_LOGI(TAG, "Connect cleanup: ble_gap_terminate rc=%d", rc);
        }
#endif
    }
    delete[] args->bda;
    delete args;
    vTaskDelete(nullptr);
}
