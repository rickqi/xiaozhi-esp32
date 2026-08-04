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
#include "host/ble_uuid.h"
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
    ESP_LOGI(TAG, "StartScan: has_pending_keyboard_=%d", has_pending_keyboard_ ? 1 : 0);
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
    // USB HID Usage Table 1.12 â€?Keyboard/Keypad page (0x07)
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

        // Extract device description from advertisement/scan-response data.
        char name[64] = "?";
        bool name_complete = false;
        if (fields.name != nullptr) {
            size_t n = fields.name_len < sizeof(name) - 1 ? fields.name_len : sizeof(name) - 1;
            memcpy(name, fields.name, n);
            name[n] = '\0';
            name_complete = fields.name_is_complete;
        }
        uint16_t appearance = fields.appearance_is_present ? fields.appearance : 0;
        bool is_keyboard = (appearance == ESP_HID_APPEARANCE_KEYBOARD);

        const char* addr_type_str = "?";
        switch (event->disc.addr.type) {
            case 0x00: addr_type_str = "public"; break;    // BLE_ADDR_TYPE_PUBLIC
            case 0x01: addr_type_str = "random"; break;    // BLE_ADDR_TYPE_RANDOM
            case 0x02: addr_type_str = "rpa-pub"; break;   // BLE_ADDR_TYPE_RPA_PUB_DEFAULT
            case 0x03: addr_type_str = "rpa-rnd"; break;   // BLE_ADDR_TYPE_RPA_RND_DEFAULT
            default: break;
        }

        // Event type tells us if this PDU is the initial advertisement or the
        // scan response (the latter usually carries the name/appearance).
        // BLE_HCI_ADV_RPT_EVTYPE_*: 0=ADV_IND 1=DIR_IND 2=SCAN_IND
        // 3=NONCONN_IND 4=SCAN_RSP
        const char* evt_str = "?";
        switch (event->disc.event_type) {
            case 0: evt_str = "ADV_IND"; break;
            case 1: evt_str = "DIR_IND"; break;
            case 2: evt_str = "SCAN_IND"; break;
            case 3: evt_str = "NONCONN_IND"; break;
            case 4: evt_str = "SCAN_RSP"; break;
            default: break;
        }

        ESP_LOGI(TAG,
                 "BLE device: %02x:%02x:%02x:%02x:%02x:%02x  name='%s'%s  addr=%s  type=%s  RSSI=%d  appearance=0x%04x%s",
                 event->disc.addr.val[0], event->disc.addr.val[1],
                 event->disc.addr.val[2], event->disc.addr.val[3],
                 event->disc.addr.val[4], event->disc.addr.val[5],
                 name, name_complete ? "" : " (incomplete)", addr_type_str, evt_str,
                 event->disc.rssi, appearance,
                 is_keyboard ? "  [KEYBOARD]" : "");

        // Print advertised service UUIDs (16/32/128-bit) if present.
        for (int i = 0; i < fields.num_uuids16; i++) {
            uint16_t u = ble_uuid_u16(&fields.uuids16[i].u);
            if (u == 0x1812) {
                ESP_LOGI(TAG, "  -> HID service (0x1812)");
            } else {
                ESP_LOGI(TAG, "  -> service UUID16: 0x%04x", u);
            }
        }
        for (int i = 0; i < fields.num_uuids32; i++) {
            ESP_LOGI(TAG, "  -> service UUID32: 0x%08lx",
                     (unsigned long)fields.uuids32[i].value);
        }
        for (int i = 0; i < fields.num_uuids128; i++) {
            const ble_uuid128_t* u = &fields.uuids128[i];
            ESP_LOGI(TAG, "  -> service UUID128: %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     u->value[15], u->value[14], u->value[13], u->value[12],
                     u->value[11], u->value[10], u->value[9], u->value[8],
                     u->value[7], u->value[6], u->value[5], u->value[4],
                     u->value[3], u->value[2], u->value[1], u->value[0]);
        }
        if (fields.tx_pwr_lvl_is_present) {
            ESP_LOGI(TAG, "  -> TX power: %d dBm", fields.tx_pwr_lvl);
        }
        if (fields.adv_itvl_is_present) {
            ESP_LOGI(TAG, "  -> adv interval: %u ms", fields.adv_itvl);
        }
        if (fields.mfg_data != nullptr && fields.mfg_data_len > 0) {
            // Manufacturer data: first 2 bytes = company ID (little-endian),
            // rest is vendor payload.
            char hex[160];
            int pos = 0;
            for (int i = 0; i < fields.mfg_data_len && pos < (int)sizeof(hex) - 3; i++) {
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", fields.mfg_data[i]);
            }
            if (fields.mfg_data_len >= 2) {
                uint16_t company = fields.mfg_data[0] | (fields.mfg_data[1] << 8);
                ESP_LOGI(TAG, "  -> mfg data: company=0x%04x len=%d [%s]", company,
                         fields.mfg_data_len, hex);
            } else {
                ESP_LOGI(TAG, "  -> mfg data: len=%d [%s]", fields.mfg_data_len, hex);
            }
        }

        if (is_keyboard && ctx->keyboard_addr_found == 0) {
            ESP_LOGI(TAG, "Keyboard selected: %s", name);
            memcpy(ctx->keyboard_addr, event->disc.addr.val, 6);
            ctx->keyboard_addr_type = event->disc.addr.type;
            ctx->keyboard_addr_found = 1;
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE: {
        ESP_LOGI(TAG, "DISC_COMPLETE: pending=%d keyboard_found=%d",
                 (ctx->self && ctx->self->has_pending_keyboard_) ? 1 : 0,
                 ctx->keyboard_addr_found);
        // If we have a pending keyboard from a previous scan, connect it now
        // (second BTSCAN = explicit connect intent). Otherwise remember the
        // first keyboard found.
        if (ctx->self && ctx->self->has_pending_keyboard_) {
            ESP_LOGI(TAG, "Scan complete, connecting to pending keyboard...");
            ctx->self->ConnectAsync(ctx->self->pending_keyboard_addr_,
                                    ctx->self->pending_keyboard_addr_type_);
            ctx->self->has_pending_keyboard_ = false;
        } else if (ctx->keyboard_addr_found) {
            ESP_LOGI(TAG, "Scan complete. Keyboard found. Send BTSCAN again to connect...");
            if (ctx->self) {
                // IMPORTANT: Do NOT auto-connect here. esp_hidh_dev_open() is
                // BLOCKING and on a reachable-but-not-pairing keyboard it
                // hangs in GATT discovery, leaking ~17KB of NimBLE connection
                // memory that breaks SELFTEST/HTTPSTART/screenshot. Instead
                // remember the address; a second BTSCAN triggers the connect.
                memcpy(ctx->self->pending_keyboard_addr_, ctx->keyboard_addr, 6);
                ctx->self->pending_keyboard_addr_type_ = ctx->keyboard_addr_type;
                ctx->self->has_pending_keyboard_ = true;
                ESP_LOGI(TAG, "Pending keyboard saved; send BTSCAN again to connect");
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
