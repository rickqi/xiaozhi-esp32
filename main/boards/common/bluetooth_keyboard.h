#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "esp_hidh.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"

/**
 * @brief BLE HID Host keyboard input (via esp_hid component, NimBLE backend)
 *
 * Connects a Bluetooth (BLE) keyboard and forwards key presses through
 * std::function callbacks, mirroring the Button class input pattern.
 *
 * MVP scope: keyboard acts as a remote control (hotkeys). Text injection
 * into the chat is a future extension (requires protocol changes).
 */
class BluetoothKeyboard {
public:
    using KeyCallback = std::function<void(uint8_t keycode, uint8_t modifier)>;
    using Callback = std::function<void()>;

    BluetoothKeyboard();
    ~BluetoothKeyboard();

    /// Initialize esp_hidh + BT controller + NimBLE host (idempotent).
    void Init();

    /// Start BLE scan for HID keyboards (appearance 0x03C1) and auto-connect.
    /// @param seconds scan duration (default 10s)
    void StartScan(uint32_t seconds = 10);

    /// Connect to a specific keyboard by address (from scan results).
    void Connect(const uint8_t* bda, uint8_t addr_type);

    /// Disconnect current keyboard.
    void Disconnect();

    /// Whether a HID device is currently connected.
    bool IsConnected() const { return dev_ != nullptr; }

    /// Whether a BLE scan is currently in progress.
    bool IsScanning() const { return scanning_; }

    /// Whether a keyboard address from a previous scan is saved and awaiting
    /// the next BTSCAN / self.scan_ble call to connect (phase 1 of two-phase).
    bool HasPendingKeyboard() const { return has_pending_keyboard_; }

    /// Name of the keyboard found by the most recent scan ("" if none).
    const std::string& LastScanName() const { return last_scan_name_; }

    /// Name of the currently connected keyboard ("" if not connected).
    const std::string& ConnectedName() const { return connected_name_; }

    /// Whether a previously connected keyboard address is persisted in NVS
    /// (auto-reconnect candidate at boot).
    bool HasSavedKeyboard() const { return has_saved_keyboard_; }

    /// Block until the current scan completes (DISC_COMPLETE) or timeout.
    /// @return true if a keyboard was found during that scan.
    bool WaitScanComplete(uint32_t timeout_ms);

    /// Persist the last successfully connected keyboard to NVS so it can be
    /// reconnected automatically after a reboot.
    void SaveLastKeyboardToNvs();

    /// Load the persisted keyboard address from NVS (boot auto-reconnect).
    void LoadLastKeyboardFromNvs();

    /// Direct-connect to the saved keyboard (boot auto-reconnect). Safe by
    /// design: connects ONLY to the known address — never triggers a scan,
    /// so it cannot leak NimBLE connection memory on unreachable devices.
    void AutoReconnect();

    // --- Callbacks (all invoked on the caller's task; see Note below) ---
    void OnKeyPress(KeyCallback callback) { on_key_press_ = std::move(callback); }
    void OnConnect(Callback callback) { on_connect_ = std::move(callback); }
    void OnDisconnect(Callback callback) { on_disconnect_ = std::move(callback); }

    /// Blocking helper for callers: maps HID keycode to ASCII char (Boot report).
    /// @note returns 0 for non-printable / unmapped keys.
    /// @note currently unused (per-key ASCII logging was removed in v3.4.6 to
    /// avoid SD-log stalls on the esp_hidh_events task); kept for future
    /// text-input use.
    [[maybe_unused]] static char KeycodeToAscii(uint8_t keycode, bool shift);

private:
    /// Scan context passed to NimBLE GAP discovery callback.
    struct BleScanCtx {
        BluetoothKeyboard* self;
        uint32_t seconds;
        uint8_t keyboard_addr[6];
        uint8_t keyboard_addr_type;
        int keyboard_addr_found;
    };

    /// Args passed to the dedicated connect task.
    struct ConnectTaskArgs {
        BluetoothKeyboard* self;
        uint8_t* bda;         // heap-allocated 6-byte address
        uint8_t addr_type;
    };

    /// esp_hidh event loop callback (runs on esp_hidh_events task).
    static void EventHandler(void* handler_args, esp_event_base_t base,
                             int32_t id, void* event_data);

    /// NimBLE host task entry (runs nimble_port_run()).
    static void NimbleHostTask(void* param);

    /// NimBLE GAP discovery callback (scan for keyboards).
    static int GapEventCallback(struct ble_gap_event* event, void* arg);

    /// Spawn a dedicated task to connect (avoids blocking NimBLE host task).
    void ConnectAsync(const uint8_t* bda, uint8_t addr_type);
    static void ConnectTask(void* arg);

    /// Parse a Boot keyboard report (8 bytes) and dispatch key presses.
    void HandleBootReport(const uint8_t* data, uint16_t len, uint8_t modifier_override);

    // --- BT controller / NimBLE host init helpers (port from blufi.cpp) ---
    esp_err_t ControllerInit();
    esp_err_t HostInit();

    esp_hidh_dev_t* dev_ = nullptr;
    bool bt_initialized_ = false;
    // True while a GAP discovery is in progress. Used by the board's log
    // tee to skip SD writes for ble_keyboard lines during scanning (the
    // NimBLE host task would otherwise be blocked on SD I/O for every
    // advertisement, which can stall HCI processing and crash the device).
    volatile bool scanning_ = false;

    // Pending keyboard address from a previous scan (connect on next BTSCAN).
    uint8_t pending_keyboard_addr_[6] = {};
    uint8_t pending_keyboard_addr_type_ = 0;
    bool has_pending_keyboard_ = false;

    // Names captured for voice/status reporting (v3.5.0).
    std::string last_scan_name_;     // keyboard found by most recent scan
    std::string connected_name_;     // currently connected keyboard

    // Last successfully connected keyboard, persisted to NVS for auto
    // reconnect at boot (safe: direct connect only, no blind scanning).
    uint8_t saved_keyboard_addr_[6] = {};
    uint8_t saved_keyboard_addr_type_ = 0;
    std::string saved_keyboard_name_;
    bool has_saved_keyboard_ = false;

    // Scan completion signalling for the blocking WaitScanComplete() used by
    // the self.scan_ble MCP tool (scan runs async on the NimBLE host task).
    SemaphoreHandle_t scan_complete_sem_ = nullptr;
    volatile bool scan_found_ = false;

    KeyCallback on_key_press_;
    Callback on_connect_;
    Callback on_disconnect_;
};
