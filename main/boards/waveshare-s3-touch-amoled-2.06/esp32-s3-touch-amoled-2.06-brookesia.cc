#include "wifi_board.h"
#include "display/brookesia_display/brookesia_display.h"
#include "esp_lcd_sh8601.h"

#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "config.h"
#include "power_save_timer.h"
#include "axp2101.h"
#include "i2c_device.h"
#include "backlight.h"
#include "bluetooth_keyboard.h"
#include "http_file_server.h"
#include "version_info.h"
#include "assets/lang_config.h"
#include <font_awesome.h>

#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include "settings.h"

#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#define TAG "BrookesiaAmoled2inch06"
#define LCD_OPCODE_WRITE_CMD (0x02ULL)

static void rounder_cb(lv_area_t* area) {
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110);
        WriteReg(0x27, 0x10);
        WriteReg(0x80, 0x01);
        WriteReg(0x90, 0x00);
        WriteReg(0x91, 0x00);
        WriteReg(0x82, (3300 - 1500) / 100);
        WriteReg(0x92, (3300 - 500) / 100);
        WriteReg(0x93, (3300 - 500) / 100);
        WriteReg(0x90, 0x03);
        WriteReg(0x64, 0x02);
        WriteReg(0x61, 0x02);
        WriteReg(0x62, 0x0A);
        WriteReg(0x63, 0x01);
    }
};

static const sh8601_lcd_init_cmd_t vendor_specific_init[] = {
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x44, (uint8_t []){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x53, (uint8_t []){0x20}, 1, 10},
    {0x63, (uint8_t []){0xFF}, 1, 10},
    {0x51, (uint8_t []){0x00}, 1, 10},
    {0x2A, (uint8_t []){0x00,0x16,0x01,0xAF}, 4, 0},
    {0x2B, (uint8_t []){0x00,0x00,0x01,0xF5}, 4, 0},
    {0x29, (uint8_t []){0x00}, 0, 10},
    {0x51, (uint8_t []){0xFF}, 1, 0},
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_lcd_panel_io_handle_t panel_io)
        : Backlight(), panel_io_(panel_io) {}
protected:
    esp_lcd_panel_io_handle_t panel_io_;
    virtual void SetBrightnessImpl(uint8_t brightness) override {
        uint8_t data[1] = {(uint8_t)((255 * brightness) / 100)};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, &data, sizeof(data));
    }
};

class BrookesiaAmoled2inch06 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    BrookesiaDisplay* display_;
    CustomBacklight* backlight_;
    PowerSaveTimer* power_save_timer_;
    lv_display_t* lv_disp_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;

#if CONFIG_USE_BLE_HID_KEYBOARD
    BluetoothKeyboard bt_keyboard_;
#endif
    esp_timer_handle_t user_timer_ = nullptr;
    char timer_message_[128] = {};

    static void TimerCallback(void* arg) {
        auto* self = static_cast<BrookesiaAmoled2inch06*>(arg);
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
        auto* disp = Board::GetInstance().GetDisplay();
        if (disp) disp->ShowNotification(self->timer_message_, 10000);
        ESP_LOGI(TAG, "Timer fired: %s", self->timer_message_);
    }

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(20);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            pmic_->PowerOff();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags = { .enable_internal_pullup = 1 },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
        buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
        buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
        buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
        buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff
                               ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeSH8601Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(
            EXAMPLE_PIN_NUM_LCD_CS, nullptr, nullptr);
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        const sh8601_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(sh8601_lcd_init_cmd_t),
            .flags = { .use_qspi_interface = 1 },
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void*)&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(panel_io, &panel_config, &panel));
        esp_lcd_panel_set_gap(panel, 0x16, 0);
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();

        lv_init();
        lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
        port_cfg.task_priority = 4;
        port_cfg.task_affinity = 1;
        port_cfg.task_stack = 16 * 1024;
        port_cfg.timer_period_ms = 30;
        ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

        lvgl_port_display_cfg_t disp_cfg = {};
        disp_cfg.io_handle = panel_io;
        disp_cfg.panel_handle = panel;
        disp_cfg.buffer_size = DISPLAY_WIDTH * 20;
        disp_cfg.double_buffer = false;
        disp_cfg.hres = DISPLAY_WIDTH;
        disp_cfg.vres = DISPLAY_HEIGHT;
        disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
        disp_cfg.flags.buff_dma = 1;
        disp_cfg.flags.buff_spiram = 0;
        disp_cfg.flags.sw_rotate = 0;
        disp_cfg.flags.swap_bytes = 1;

        lv_disp_ = lvgl_port_add_disp(&disp_cfg);
        panel_io_ = panel_io;
        assert(lv_disp_);
    }

    void InitBrookesiaDisplay() {
        display_ = new BrookesiaDisplay(lv_disp_, panel_io_,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT);
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = GPIO_NUM_9,
            .int_gpio_num = GPIO_NUM_38,
            .levels = { .reset = 0, .interrupt = 0 },
            .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
        tp_io_config.scl_speed_hz = 400 * 1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Init touch FT5x06");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp));
        ESP_LOGI(TAG, "FT5x06 init OK, adding to lvgl_port");
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch registered with LVGL");
    }

#if CONFIG_USE_BLE_HID_KEYBOARD
    void InitializeBleKeyboard() {
        bt_keyboard_.Init();
        bt_keyboard_.OnKeyPress([this](uint8_t keycode, uint8_t modifier) {
            HandleKeyboardKey(keycode, modifier);
        });
        bt_keyboard_.OnConnect([this]() {
            auto* disp = Board::GetInstance().GetDisplay();
            if (disp) disp->SetBluetoothIcon(FONT_AWESOME_BLUETOOTH);
        });
        bt_keyboard_.OnDisconnect([this]() {
            auto* disp = Board::GetInstance().GetDisplay();
            if (disp) disp->SetBluetoothIcon("");
        });
        if (bt_keyboard_.HasSavedKeyboard()) {
            bt_keyboard_.AutoReconnect();
        }
    }

    void HandleKeyboardKey(uint8_t keycode, uint8_t) {
        auto& app = Application::GetInstance();
        auto* codec = Board::GetInstance().GetAudioCodec();
        switch (keycode) {
        case 0x28:
            app.ToggleChatState();
            break;
        case 0x29:
            app.AbortSpeaking(kAbortReasonNone);
            break;
        case 0x2C:
            app.ToggleChatState();
            break;
        case 0x52:
            if (codec) codec->SetOutputVolume(codec->output_volume() + 10);
            break;
        case 0x51:
            if (codec) codec->SetOutputVolume(codec->output_volume() - 10);
            break;
        case 0x4D:
            if (codec) codec->SetOutputVolume(0);
            break;
        default:
            break;
        }
    }
#endif

    void InitializeSdCard() {
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.flags = SDMMC_HOST_FLAG_1BIT;
        host.slot = SDMMC_HOST_SLOT_1;
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_config.clk = (gpio_num_t)SDMMC_CLK_PIN;
        slot_config.cmd = (gpio_num_t)SDMMC_CMD_PIN;
        slot_config.d0  = (gpio_num_t)SDMMC_D0_PIN;
        slot_config.width = 1;
        esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
        mount_config.format_if_mount_failed = false;
        mount_config.max_files = 5;
        mount_config.allocation_unit_size = 16 * 1024;
        sdmmc_card_t* card = nullptr;
        esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config,
                                                  &mount_config, &card);
        if (ret == ESP_OK) {
            sdmmc_card_print_info(stdout, card);
            mkdir("/sdcard/records", 0755);
            mkdir("/sdcard/music", 0755);
            mkdir("/sdcard/logs", 0755);
            ESP_LOGI(TAG, "SD card mounted at /sdcard");
        } else {
            ESP_LOGW(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        }
    }

    void InitializeTools() {
        auto& mcp = McpServer::GetInstance();

        mcp.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList&) {
                EnterWifiConfigMode();
                return true;
            });

        mcp.AddTool("self.get_version_info",
            "Get firmware version, build info, and feature list.\n"
            "Use when user asks about version, features, or build info.",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                return VersionInfo::BuildVersionInfoJson();
            });

        mcp.AddTool("self.get_mcp_help",
            "Get usage help for MCP tools. Optional tool_name for specific tool help.\n"
            "Use when user asks for help or tool list.",
            PropertyList({
                Property("tool_name", kPropertyTypeString, std::string(""))
            }),
            [](const PropertyList& props) -> ReturnValue {
                std::string name = props["tool_name"].value<std::string>();
                auto& srv = McpServer::GetInstance();
                if (name.empty()) return srv.GetAllToolsHelp();
                return srv.GetToolHelp(name);
            });

        mcp.AddTool("self.get_voice_commands",
            "List all voice commands supported by this device.",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                return std::string(
                    "本设备支持以下语音命令："
                    "一、对话控制：说「你好小智」唤醒设备；"
                    "说「停止」中断回复。"
                    "二、蓝牙键盘：说「扫描蓝牙键盘」，扫描并连接键盘；"
                    "说「键盘连上了吗」，查询连接状态。"
                    "三、状态查询：说「电量多少」，查询电池；"
                    "说「你的版本是多少」，查看固件版本与功能。"
                    "四、屏幕控制：说「调亮屏幕」或「调暗屏幕」，调节亮度。"
                    "五、系统管理：说「重新配网」，进入 WiFi 配网模式；"
                    "说「设置一个十分钟的定时器」，设定倒计时提醒。");
            });

        mcp.AddTool("self.set_timer",
            "Set a countdown timer. Plays sound and shows notification when expired.\n"
            "duration_minutes: 1-1440 (default 5). message: shown when timer fires.",
            PropertyList({
                Property("duration_minutes", kPropertyTypeInteger, 5, 1, 1440),
                Property("message", kPropertyTypeString, std::string("时间到了"))
            }),
            [this](const PropertyList& props) -> ReturnValue {
                int mins = props["duration_minutes"].value<int>();
                std::string msg = props["message"].value<std::string>();
                if (user_timer_) {
                    esp_timer_stop(user_timer_);
                    esp_timer_delete(user_timer_);
                }
                snprintf(timer_message_, sizeof(timer_message_), "%s", msg.c_str());
                esp_timer_create_args_t args = {};
                args.callback = TimerCallback;
                args.arg = this;
                args.name = "user_timer";
                esp_timer_create(&args, &user_timer_);
                esp_timer_start_once(user_timer_, (int64_t)mins * 60 * 1000000LL);
                char ret[160];
                snprintf(ret, sizeof(ret), "Timer set for %d minutes: %s", mins, msg.c_str());
                return std::string(ret);
            });

        mcp.AddTool("self.start_file_server",
            "Start WiFi HTTP file server for wireless file download via browser.",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                auto& srv = HttpFileServer::GetInstance();
                if (srv.IsRunning()) return std::string("Already running: " + srv.GetUrl());
                if (srv.Start(80)) return std::string("Started: " + srv.GetUrl());
                return std::string("Error: failed to start (WiFi connected?)");
            });

        mcp.AddTool("self.stop_file_server",
            "Stop the WiFi HTTP file server.",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                auto& srv = HttpFileServer::GetInstance();
                if (srv.IsRunning()) { srv.Stop(); return std::string("Stopped"); }
                return std::string("Was not running");
            });

        mcp.AddTool("self.list_recordings",
            "List voice recordings saved on the SD card (/sdcard/records/).",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                cJSON* root = cJSON_CreateArray();
                DIR* dir = opendir("/sdcard/records");
                if (!dir) return root;
                struct dirent* ent;
                while ((ent = readdir(dir)) != nullptr) {
                    if (strstr(ent->d_name, ".wav") == nullptr) continue;
                    cJSON* item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "filename", ent->d_name);
                    std::string path = "/sdcard/records/" + std::string(ent->d_name);
                    struct stat st;
                    if (stat(path.c_str(), &st) == 0) {
                        cJSON_AddNumberToObject(item, "size", st.st_size);
                    }
                    cJSON_AddItemToArray(root, item);
                }
                closedir(dir);
                return root;
            });

        mcp.AddTool("self.list_music",
            "List music files on the SD card (/sdcard/music/).",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                cJSON* root = cJSON_CreateArray();
                DIR* dir = opendir("/sdcard/music");
                if (!dir) return root;
                struct dirent* ent;
                while ((ent = readdir(dir)) != nullptr) {
                    if (strstr(ent->d_name, ".mp3") == nullptr &&
                        strstr(ent->d_name, ".wav") == nullptr) continue;
                    cJSON_AddItemToArray(root, cJSON_CreateString(ent->d_name));
                }
                closedir(dir);
                return root;
            });

        mcp.AddTool("self.list_chatlogs",
            "List chat conversation logs on the SD card.",
            PropertyList({
                Property("directory", kPropertyTypeString, std::string("chatlogs"))
            }),
            [](const PropertyList& props) -> ReturnValue {
                std::string dir_param = props["directory"].value<std::string>();
                std::string path = (dir_param == "system_logs" || dir_param == "logs")
                    ? "/sdcard/logs" : "/sdcard/logs/chatlogs";
                cJSON* root = cJSON_CreateArray();
                DIR* dir = opendir(path.c_str());
                if (!dir) return root;
                struct dirent* ent;
                while ((ent = readdir(dir)) != nullptr) {
                    if (ent->d_name[0] == '.') continue;
                    cJSON_AddItemToArray(root, cJSON_CreateString(ent->d_name));
                }
                closedir(dir);
                return root;
            });

#if CONFIG_USE_BLE_HID_KEYBOARD
        mcp.AddTool("self.scan_ble",
            "Scan for BLE devices to connect a bluetooth keyboard.\n"
            "Put keyboard in pairing mode first, then call. Second call connects.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                if (bt_keyboard_.IsConnected()) {
                    auto name = bt_keyboard_.ConnectedName();
                    return std::string("蓝牙键盘已连接：") +
                           (name.empty() ? std::string("未知") : name);
                }
                if (bt_keyboard_.IsScanning())
                    return std::string("扫描中，约10秒后再问。");
                bt_keyboard_.StartScan(10);
                return std::string("已开始扫描，约10秒后再说「扫描蓝牙键盘」连接。");
            });

        mcp.AddTool("self.get_ble_keyboard_status",
            "Query bluetooth keyboard connection status.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                if (bt_keyboard_.IsConnected()) {
                    auto name = bt_keyboard_.ConnectedName();
                    return std::string("已连接：") +
                           (name.empty() ? std::string("未知") : name);
                }
                if (bt_keyboard_.IsScanning())
                    return std::string("正在扫描中。");
                return std::string("未连接。请将键盘进入配对模式后说「扫描蓝牙键盘」。");
            });

        mcp.AddTool("self.get_ble_keyboard_shortcuts",
            "List bluetooth keyboard shortcut keys.",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                return std::string(
                    "快捷键：回车=对话切换，Esc=停止，空格=倾听，"
                    "↑↓=音量±10，M=静音，Tab=提示音。");
            });
#endif
    }

public:
    BrookesiaAmoled2inch06() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
        InitializeAxp2101();
        InitializeSpi();
        InitializeSH8601Display();
        InitializeTouch();
        InitBrookesiaDisplay();
        InitializeButtons();
        InitializeSdCard();
#if CONFIG_USE_BLE_HID_KEYBOARD
        InitializeBleKeyboard();
#endif
        InitializeTools();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }
    virtual Backlight* GetBacklight() override { return backlight_; }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(BrookesiaAmoled2inch06);
