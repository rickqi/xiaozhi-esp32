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
#include <sys/unistd.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include <mutex>
#include <ssid_manager.h>
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"
#include "device_state.h"

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
    esp_lcd_panel_handle_t panel_ = nullptr;

#if CONFIG_USE_BLE_HID_KEYBOARD
    BluetoothKeyboard bt_keyboard_;
#endif
    esp_timer_handle_t user_timer_ = nullptr;
    char timer_message_[128] = {};

    volatile bool recording_ = false;
    volatile uint32_t recording_until_ms_ = 0;
    volatile bool music_playing_ = false;
    volatile bool music_stop_ = false;
    TaskHandle_t music_task_ = nullptr;
    SemaphoreHandle_t music_req_sem_ = nullptr;
    static constexpr size_t kMusicTaskStackBytes = 16 * 1024;
    char music_request_path_[300] = "";
    volatile bool chatlog_playing_ = false;
    volatile bool chatlog_stop_ = false;
    int chatlog_channel_mode_ = 0;
    bool self_test_running_ = false;
    std::string self_test_result_;
    std::mutex self_test_mutex_;

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

        std::vector<uint16_t> white_buf(DISPLAY_WIDTH, 0xFFFF);
        for (int y = 0; y < DISPLAY_HEIGHT; y++) {
            esp_lcd_panel_draw_bitmap(panel, 0, y, DISPLAY_WIDTH, y + 1, white_buf.data());
        }

        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();

        lv_init();
        lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
        port_cfg.task_priority = 1;
        port_cfg.task_affinity = 1;
        port_cfg.task_stack = 16 * 1024;
        port_cfg.timer_period_ms = 500;
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
        panel_ = panel;
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

    bool InitializeSdCard() {
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
            mkdir("/sdcard/logs/chatlogs", 0755);
            ESP_LOGI(TAG, "SD card mounted at /sdcard");
            return true;
        }
        ESP_LOGW(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        return false;
    }

    void ShowNotify(const char* msg) { ShowNotify(msg, 3000); }
    void ShowNotify(const char* msg, int duration_ms) {
        auto display = Board::GetInstance().GetDisplay();
        if (display) display->ShowNotification(msg, duration_ms);
    }

    void ResumeAudioService() {
        auto& app = Application::GetInstance();
        auto& audio = app.GetAudioService();
        audio.Start();
        DeviceState state = app.GetDeviceState();
        if (state == kDeviceStateIdle) {
            audio.EnableVoiceProcessing(false);
            audio.EnableWakeWordDetection(true);
        } else if (state == kDeviceStateListening) {
            audio.EnableVoiceProcessing(true);
            audio.EnableWakeWordDetection(false);
        }
    }

    static void RecordTask(void* arg) {
        auto* board = static_cast<BrookesiaAmoled2inch06*>(arg);
        board->RecordTaskImpl();
        vTaskDelete(NULL);
    }

    void RecordTaskImpl() {
        auto& app = Application::GetInstance();
        auto& audio = app.GetAudioService();
        auto codec = Board::GetInstance().GetAudioCodec();

        audio.Stop();
        vTaskDelay(pdMS_TO_TICKS(100));
        codec->EnableInput(true);
        codec->EnableOutput(false);

        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char path[64];
        snprintf(path, sizeof(path), "/sdcard/records/rec_%04d%02d%02d_%02d%02d%02d.wav",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);

        const int sample_rate = 24000;
        const int channels = 2;
        const int bits = 16;
        uint32_t data_len = 0;
        FILE* f = fopen(path, "wb");
        if (!f) {
            ESP_LOGE(TAG, "Cannot create recording file");
            recording_ = false;
            ShowNotify("Rec Failed");
            ResumeAudioService();
            return;
        }
        uint8_t hdr[44] = {0};
        memcpy(hdr, "RIFF", 4);
        memcpy(hdr + 8, "WAVE", 4);
        memcpy(hdr + 12, "fmt ", 4);
        hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;
        hdr[20] = 1; hdr[21] = 0;
        hdr[22] = channels & 0xFF; hdr[23] = (channels >> 8) & 0xFF;
        hdr[24] = sample_rate & 0xFF; hdr[25] = (sample_rate >> 8) & 0xFF;
        hdr[26] = (sample_rate >> 16) & 0xFF; hdr[27] = (sample_rate >> 24) & 0xFF;
        uint32_t byte_rate = sample_rate * channels * bits / 8;
        hdr[28] = byte_rate & 0xFF; hdr[29] = (byte_rate >> 8) & 0xFF;
        hdr[30] = (byte_rate >> 16) & 0xFF; hdr[31] = (byte_rate >> 24) & 0xFF;
        hdr[32] = channels * bits / 8;
        hdr[34] = bits;
        memcpy(hdr + 36, "data", 4);
        fwrite(hdr, 1, 44, f);

        const int chunk_samples = 512;
        std::vector<int16_t> pcm(chunk_samples * channels);
        while (recording_) {
            uint32_t now_ms = esp_timer_get_time() / 1000;
            if (recording_until_ms_ > 0 && now_ms >= recording_until_ms_) {
                recording_ = false;
                break;
            }
            if (codec->InputData(pcm)) {
                fwrite(pcm.data(), 1, pcm.size() * sizeof(int16_t), f);
                data_len += pcm.size() * sizeof(int16_t);
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        recording_until_ms_ = 0;

        fseek(f, 4, SEEK_SET);
        uint32_t riff_size = 36 + data_len;
        fwrite(&riff_size, 1, 4, f);
        fseek(f, 40, SEEK_SET);
        fwrite(&data_len, 1, 4, f);
        fclose(f);

        codec->EnableInput(false);
        ResumeAudioService();

        float dur = (float)data_len / (sample_rate * channels * 2);
        char info[64];
        snprintf(info, sizeof(info), "Saved %u.%02us %luKB %s",
                 (int)dur, (int)(dur * 100) % 100,
                 (unsigned long)(data_len / 1024), strrchr(path, '/') + 1);
        ShowNotify(info, 4000);
        ESP_LOGI(TAG, "Recording done: %s (%u bytes, %.1fs)", path, (unsigned int)data_len, dur);
    }

    bool StartRecording(int duration_sec) {
        if (recording_) return false;
        if (!InitializeSdCard()) return false;
        if (duration_sec <= 0) duration_sec = 5;
        if (duration_sec > 120) duration_sec = 120;
        recording_until_ms_ = (esp_timer_get_time() / 1000) + duration_sec * 1000;
        recording_ = true;
        xTaskCreatePinnedToCore(RecordTask, "sd_record", 8 * 1024, this, 3, NULL, 1);
        return true;
    }

    void PlayRecordingPath(const char* path) {
        auto& app = Application::GetInstance();
        auto& audio = app.GetAudioService();
        auto codec = Board::GetInstance().GetAudioCodec();

        audio.Stop();
        vTaskDelay(pdMS_TO_TICKS(100));
        codec->EnableOutput(true);
        codec->EnableInput(false);

        FILE* f = fopen(path, "rb");
        if (!f) {
            ESP_LOGE(TAG, "Playback: cannot open %s errno=%d", path, errno);
            ShowNotify("Play Failed");
            ResumeAudioService();
            return;
        }
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 44, SEEK_SET);
        long data_size = (file_size > 44) ? (file_size - 44) : 0;

        std::vector<int16_t> stereo(1024 * 2);
        std::vector<int16_t> mono(1024);
        size_t n;
        long played = 0;
        int last_pct = -1;
        ShowNotify("Playing... 0%");
        while ((n = fread(stereo.data(), 1, stereo.size() * sizeof(int16_t), f)) > 0) {
            size_t frames = n / (2 * sizeof(int16_t));
            for (size_t i = 0; i < frames; i++) {
                mono[i] = stereo[i * 2];
            }
            mono.resize(frames);
            codec->OutputData(mono);
            mono.resize(1024);
            played += n;
            int pct = (data_size > 0) ? (int)(played * 100 / data_size) : 100;
            if (pct >= last_pct + 25) {
                last_pct = (pct / 25) * 25;
                char prog[32];
                snprintf(prog, sizeof(prog), "Playing... %d%%", last_pct);
                ShowNotify(prog, 3000);
            }
        }
        fclose(f);
        codec->EnableOutput(false);
        ResumeAudioService();
        ShowNotify("Play Done");
    }

    bool PlayRecordingByName(const char* filename) {
        if (!InitializeSdCard()) return false;
        char path[256];
        snprintf(path, sizeof(path), "/sdcard/records/%.200s", filename);
        struct stat st;
        if (stat(path, &st) != 0) return false;
        static char play_path[256];
        strncpy(play_path, path, sizeof(play_path) - 1);
        play_path[sizeof(play_path) - 1] = '\0';
        xTaskCreatePinnedToCore([](void* arg) {
            auto* board = static_cast<BrookesiaAmoled2inch06*>(arg);
            board->PlayRecordingPath(play_path);
            vTaskDelete(NULL);
        }, "mcp_play", 8 * 1024, this, 3, NULL, 1);
        return true;
    }

    bool DeleteRecordingByName(const char* filename) {
        if (!InitializeSdCard()) return false;
        char path[256];
        snprintf(path, sizeof(path), "/sdcard/records/%.200s", filename);
        struct stat st;
        if (stat(path, &st) != 0) return false;
        int ret = unlink(path);
        if (ret == 0) {
            ESP_LOGI(TAG, "Deleted recording: %s", path);
            return true;
        }
        ESP_LOGE(TAG, "Delete recording failed %s errno=%d", path, errno);
        return false;
    }

    cJSON* ListRecordingsJson() {
        cJSON* json = cJSON_CreateObject();
        cJSON* files = cJSON_CreateArray();
        if (!InitializeSdCard()) {
            cJSON_AddStringToObject(json, "error", "no SD card");
            cJSON_AddItemToObject(json, "recordings", files);
            return json;
        }
        DIR* dir = opendir("/sdcard/records");
        if (!dir) {
            cJSON_AddStringToObject(json, "error", "no records directory");
            cJSON_AddItemToObject(json, "recordings", files);
            return json;
        }
        struct { char name[128]; long size; float duration; } list[20];
        int count = 0;
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL && count < 20) {
            if (strncmp(ent->d_name, "rec_", 4) == 0 && strstr(ent->d_name, ".wav")) {
                char path[256];
                snprintf(path, sizeof(path), "/sdcard/records/%.200s", ent->d_name);
                struct stat st;
                long size = 0;
                if (stat(path, &st) == 0) size = st.st_size;
                float dur = (size > 44) ? (float)(size - 44) / (24000 * 2 * 2) : 0.0f;
                snprintf(list[count].name, sizeof(list[count].name), "%.*s",
                         (int)sizeof(list[count].name) - 1, ent->d_name);
                list[count].size = size;
                list[count].duration = dur;
                count++;
            }
        }
        closedir(dir);
        int max_show = count < 10 ? count : 10;
        for (int i = count - 1, j = 0; i >= 0 && j < max_show; i--, j++) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", list[i].name);
            cJSON_AddNumberToObject(item, "size", list[i].size);
            cJSON_AddNumberToObject(item, "duration_seconds", list[i].duration);
            cJSON_AddItemToArray(files, item);
        }
        cJSON_AddNumberToObject(json, "count", count);
        cJSON_AddItemToObject(json, "recordings", files);
        return json;
    }

    cJSON* ListMusicJson() {
        cJSON* json = cJSON_CreateObject();
        cJSON* files = cJSON_CreateArray();
        if (!InitializeSdCard()) {
            cJSON_AddStringToObject(json, "error", "no SD card");
            cJSON_AddItemToObject(json, "music", files);
            return json;
        }
        DIR* dir = opendir("/sdcard/music");
        if (!dir) {
            cJSON_AddStringToObject(json, "error", "no music directory");
            cJSON_AddItemToObject(json, "music", files);
            return json;
        }
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strstr(ent->d_name, ".mp3") || strstr(ent->d_name, ".MP3") ||
                strstr(ent->d_name, ".m4a") || strstr(ent->d_name, ".M4A") ||
                strstr(ent->d_name, ".aac") || strstr(ent->d_name, ".AAC") ||
                strstr(ent->d_name, ".wav") || strstr(ent->d_name, ".WAV")) {
                cJSON* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "name", ent->d_name);
                char path[300];
                snprintf(path, sizeof(path), "/sdcard/music/%.240s", ent->d_name);
                struct stat st;
                if (stat(path, &st) == 0) {
                    cJSON_AddNumberToObject(item, "size", st.st_size);
                }
                cJSON_AddItemToArray(files, item);
            }
        }
        closedir(dir);
        cJSON_AddItemToObject(json, "music", files);
        return json;
    }

    static std::vector<int16_t> LinearResample(const std::vector<int16_t>& in,
                                               int src_rate, int dst_rate) {
        if (src_rate <= 0 || dst_rate <= 0 || src_rate == dst_rate) {
            return in;
        }
        size_t in_len = in.size();
        size_t out_len = (size_t)(((uint64_t)in_len * dst_rate) / src_rate);
        if (out_len == 0) out_len = 1;
        std::vector<int16_t> out(out_len);
        if (in_len == 1) {
            std::fill(out.begin(), out.end(), in[0]);
            return out;
        }
        double step = (double)src_rate / (double)dst_rate;
        for (size_t i = 0; i < out_len; i++) {
            double pos = (double)i * step;
            size_t i0 = (size_t)pos;
            size_t i1 = i0 + 1;
            if (i0 >= in_len) { out[i] = in[in_len - 1]; continue; }
            if (i1 >= in_len) { out[i] = in[i0]; continue; }
            double frac = pos - (double)i0;
            out[i] = (int16_t)(in[i0] * (1.0 - frac) + in[i1] * frac);
        }
        return out;
    }

    bool PlayMusicPath(const char* path) {
        auto& app = Application::GetInstance();
        auto& audio = app.GetAudioService();
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr) {
            ShowNotify("Music: no codec");
            return false;
        }

        audio.Stop();
        vTaskDelay(pdMS_TO_TICKS(100));
        codec->EnableOutput(true);
        codec->EnableInput(false);

        FILE* f = fopen(path, "rb");
        if (!f) {
            ESP_LOGE(TAG, "Music: cannot open %s errno=%d", path, errno);
            ShowNotify("Music Failed");
            codec->EnableOutput(false);
            ResumeAudioService();
            music_playing_ = false;
            return false;
        }
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);

        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();

        esp_audio_simple_dec_cfg_t dec_cfg = {};
        dec_cfg.dec_cfg = nullptr;
        dec_cfg.cfg_size = 0;
        dec_cfg.use_frame_dec = false;
        const char* codec_name = "MP3";
        if (strstr(path, ".aac") || strstr(path, ".AAC") ||
            strstr(path, ".m4a") || strstr(path, ".M4A")) {
            dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
            codec_name = "M4A";
        } else {
            dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        }
        esp_audio_simple_dec_handle_t decoder = nullptr;
        if (esp_audio_simple_dec_open(&dec_cfg, &decoder) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Music: failed to open %s decoder", codec_name);
            fclose(f);
            codec->EnableOutput(false);
            ResumeAudioService();
            music_playing_ = false;
            ShowNotify("Music Failed");
            return false;
        }

        const int kReadSize = 4096;
        const int kOutBytes = 16384;
        uint8_t* in_buf = (uint8_t*)heap_caps_malloc(kReadSize, MALLOC_CAP_SPIRAM);
        uint8_t* out_buf = (uint8_t*)heap_caps_malloc(kOutBytes, MALLOC_CAP_SPIRAM);
        if (!in_buf || !out_buf) {
            ESP_LOGE(TAG, "Music: buffer alloc failed");
            if (in_buf) free(in_buf);
            if (out_buf) free(out_buf);
            esp_audio_simple_dec_close(decoder);
            fclose(f);
            codec->EnableOutput(false);
            ResumeAudioService();
            music_playing_ = false;
            ShowNotify("Music Failed");
            return false;
        }

        int mp3_rate = 44100, mp3_ch = 2;
        bool need_resample = false;
        bool info_ready = false;
        uint32_t decoded_total = 0;
        uint32_t last_notify_ms = 0;
        music_stop_ = false;
        music_playing_ = true;

        const char* slash = strrchr(path, '/');
        const char* song = slash ? slash + 1 : path;
        char song_info[96];
        snprintf(song_info, sizeof(song_info), ">> %s", song);
        ShowNotify(song_info, 8000);

        while (!music_stop_) {
            int rd = fread(in_buf, 1, kReadSize, f);
            if (rd <= 0) break;

            esp_audio_simple_dec_raw_t raw = {};
            raw.buffer = in_buf;
            raw.len = rd;
            raw.eos = (rd < kReadSize);
            raw.frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE;

            while (raw.len > 0 && !music_stop_) {
                esp_audio_simple_dec_out_t out = {};
                out.buffer = out_buf;
                out.len = kOutBytes;

                esp_audio_err_t ret = esp_audio_simple_dec_process(decoder, &raw, &out);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    break;
                }
                if (ret != ESP_AUDIO_ERR_OK) {
                    break;
                }

                if (out.decoded_size > 0) {
                    if (!info_ready) {
                        esp_audio_simple_dec_info_t info = {};
                        if (esp_audio_simple_dec_get_info(decoder, &info) == ESP_AUDIO_ERR_OK) {
                            mp3_rate = info.sample_rate;
                            mp3_ch = info.channel;
                        }
                        need_resample = (mp3_rate != codec->output_sample_rate());
                        info_ready = true;
                    }
                    decoded_total += out.decoded_size;
                    int samples = out.decoded_size / sizeof(int16_t);
                    int16_t* pcm = (int16_t*)out_buf;

                    std::vector<int16_t> mono;
                    if (mp3_ch >= 2) {
                        mono.resize(samples / 2);
                        for (int i = 0, j = 0; i + 1 < samples; i += 2, j++) {
                            mono[j] = (int16_t)(((int)pcm[i] + pcm[i + 1]) / 2);
                        }
                    } else {
                        mono.assign(pcm, pcm + samples);
                    }
                    if (!mono.empty()) {
                        if (need_resample) {
                            std::vector<int16_t> resampled = LinearResample(mono, mp3_rate, codec->output_sample_rate());
                            codec->OutputData(resampled);
                        } else {
                            codec->OutputData(mono);
                        }
                        uint32_t now_ms = esp_timer_get_time() / 1000;
                        if (now_ms - last_notify_ms >= 5000) {
                            last_notify_ms = now_ms;
                            long cur = ftell(f);
                            int pct = (file_size > 0) ? (int)((cur * 100) / file_size) : 0;
                            if (pct > 100) pct = 100;
                            char prog[96];
                            snprintf(prog, sizeof(prog), "%s  %d%%", song, pct);
                            ShowNotify(prog, 5000);
                        }
                    }
                }

                if (raw.consumed == 0) break;
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;
            }
            if (raw.eos) break;
        }

        music_playing_ = false;
        music_stop_ = false;
        esp_audio_simple_dec_close(decoder);
        free(in_buf);
        free(out_buf);
        fclose(f);

        codec->EnableOutput(false);
        ResumeAudioService();
        ESP_LOGI(TAG, "Music play done: %s (%u bytes decoded)", path, (unsigned int)decoded_total);
        ShowNotify(music_stop_ ? "Music Stopped" : "Music Done");
        return true;
    }

    void MusicTask() {
        while (true) {
            if (music_req_sem_ == nullptr ||
                xSemaphoreTake(music_req_sem_, portMAX_DELAY) != pdTRUE) {
                continue;
            }
            PlayMusicPath(music_request_path_);
        }
    }

    bool PlayMusicByName(const char* filename) {
        if (music_playing_) return false;
        if (!InitializeSdCard()) return false;
        char path[300];
        snprintf(path, sizeof(path), "/sdcard/music/%.240s", filename);
        struct stat st;
        if (stat(path, &st) != 0) return false;

        if (music_task_ == nullptr) {
            music_req_sem_ = xSemaphoreCreateBinary();
            if (music_req_sem_ == nullptr) return false;
            static StackType_t* music_stack =
                (StackType_t*)heap_caps_malloc(kMusicTaskStackBytes, MALLOC_CAP_SPIRAM);
            if (music_stack == nullptr) return false;
            static StaticTask_t music_tcb;
            music_task_ = xTaskCreateStaticPinnedToCore(
                [](void* arg) {
                    auto* board = static_cast<BrookesiaAmoled2inch06*>(arg);
                    board->MusicTask();
                },
                "mcp_music", kMusicTaskStackBytes / sizeof(StackType_t), this,
                3, music_stack, &music_tcb, 1);
            if (music_task_ == nullptr) return false;
            ESP_LOGI(TAG, "Music: resident playback task created (PSRAM stack)");
        }

        snprintf(music_request_path_, sizeof(music_request_path_), "%s", path);
        xSemaphoreGive(music_req_sem_);
        return true;
    }

    void StopMusic() { music_stop_ = true; }

    bool DeleteMusicByName(const char* filename) {
        if (!InitializeSdCard()) return false;
        char path[300];
        snprintf(path, sizeof(path), "/sdcard/music/%.240s", filename);
        struct stat st;
        if (stat(path, &st) != 0) return false;
        int ret = unlink(path);
        if (ret == 0) {
            ESP_LOGI(TAG, "Deleted music: %s", path);
            return true;
        }
        ESP_LOGE(TAG, "Delete music failed %s errno=%d", path, errno);
        return false;
    }

    static void BuildChatlogPath(char* buf, size_t buf_size, const char* filename, const char* ext) {
        char base[160];
        strncpy(base, filename, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        char* dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        snprintf(buf, buf_size, "/sdcard/logs/chatlogs/%.*s.%s",
                 (int)(sizeof(base) - 1), base, ext);
    }

    static std::string TopicFromChatlogName(const char* filename) {
        const char* p = filename;
        int underscores = 0;
        for (; *p; p++) {
            if (*p == '_') {
                underscores++;
                if (underscores == 2) { p++; break; }
            }
        }
        if (underscores == 2 && *p) {
            std::string topic(p);
            size_t dot = topic.find_last_of('.');
            if (dot != std::string::npos) topic = topic.substr(0, dot);
            return topic;
        }
        return "chat";
    }

    cJSON* ListChatlogsJson() {
        cJSON* json = cJSON_CreateObject();
        cJSON* files = cJSON_CreateArray();
        if (!InitializeSdCard()) {
            cJSON_AddStringToObject(json, "error", "no SD card");
            cJSON_AddItemToObject(json, "chatlogs", files);
            return json;
        }
        DIR* dir = opendir("/sdcard/logs/chatlogs");
        if (!dir) {
            cJSON_AddStringToObject(json, "error", "no chatlogs directory");
            cJSON_AddItemToObject(json, "chatlogs", files);
            return json;
        }
        struct { char name[160]; long txt_size; long wav_size; float duration; char started_at[24]; } list[20];
        int count = 0;
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL && count < 20) {
            if (strncmp(ent->d_name, "chat_", 5) == 0 && strstr(ent->d_name, ".txt")) {
                char txt_path[256];
                BuildChatlogPath(txt_path, sizeof(txt_path), ent->d_name, "txt");
                struct stat st;
                long txt_size = 0;
                if (stat(txt_path, &st) == 0) txt_size = st.st_size;
                char wav_path[256];
                BuildChatlogPath(wav_path, sizeof(wav_path), ent->d_name, "wav");
                long wav_size = 0;
                float dur = 0.0f;
                if (stat(wav_path, &st) == 0) {
                    wav_size = st.st_size;
                    dur = (wav_size > 44) ? (float)(wav_size - 44) / (24000 * 2 * 2) : 0.0f;
                }
                char started_at[24] = "";
                FILE* tf = fopen(txt_path, "r");
                if (tf) {
                    char first_line[512];
                    if (fgets(first_line, sizeof(first_line), tf)) {
                        cJSON* line = cJSON_Parse(first_line);
                        if (line) {
                            cJSON* ts = cJSON_GetObjectItem(line, "ts");
                            if (cJSON_IsString(ts)) {
                                strncpy(started_at, ts->valuestring, sizeof(started_at) - 1);
                            }
                            cJSON_Delete(line);
                        }
                    }
                    fclose(tf);
                }
                snprintf(list[count].name, sizeof(list[count].name), "%.*s",
                         (int)sizeof(list[count].name) - 1, ent->d_name);
                list[count].txt_size = txt_size;
                list[count].wav_size = wav_size;
                list[count].duration = dur;
                snprintf(list[count].started_at, sizeof(list[count].started_at), "%s", started_at);
                count++;
            }
        }
        closedir(dir);
        int max_show = count < 10 ? count : 10;
        for (int i = count - 1, j = 0; i >= 0 && j < max_show; i--, j++) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", list[i].name);
            cJSON_AddStringToObject(item, "topic", TopicFromChatlogName(list[i].name).c_str());
            cJSON_AddNumberToObject(item, "txt_size", list[i].txt_size);
            cJSON_AddNumberToObject(item, "wav_size", list[i].wav_size);
            cJSON_AddNumberToObject(item, "duration_seconds", list[i].duration);
            if (list[i].started_at[0]) {
                cJSON_AddStringToObject(item, "started_at", list[i].started_at);
            }
            cJSON_AddItemToArray(files, item);
        }
        cJSON_AddNumberToObject(json, "count", count);
        cJSON_AddItemToObject(json, "chatlogs", files);
        return json;
    }

    cJSON* GetChatlogSummaryJson(const char* filename) {
        cJSON* json = cJSON_CreateObject();
        cJSON* turns = cJSON_CreateArray();
        if (!InitializeSdCard()) {
            cJSON_AddStringToObject(json, "error", "no SD card");
            cJSON_AddItemToObject(json, "turns", turns);
            return json;
        }
        char txt_path[256];
        BuildChatlogPath(txt_path, sizeof(txt_path), filename, "txt");
        FILE* f = fopen(txt_path, "r");
        if (!f) {
            cJSON_AddStringToObject(json, "error", "file not found");
            cJSON_AddItemToObject(json, "turns", turns);
            return json;
        }
        cJSON_AddStringToObject(json, "filename", filename);
        cJSON_AddStringToObject(json, "topic", TopicFromChatlogName(filename).c_str());
        char line[1024];
        int turn_count = 0;
        while (fgets(line, sizeof(line), f) && turn_count < 50) {
            cJSON* entry = cJSON_Parse(line);
            if (!entry) continue;
            cJSON* role = cJSON_GetObjectItem(entry, "role");
            cJSON* text = cJSON_GetObjectItem(entry, "text");
            if (cJSON_IsString(role) && cJSON_IsString(text)) {
                cJSON* turn = cJSON_CreateObject();
                cJSON_AddStringToObject(turn, "role", role->valuestring);
                cJSON_AddStringToObject(turn, "text", text->valuestring);
                cJSON_AddItemToArray(turns, turn);
                turn_count++;
            }
            cJSON_Delete(entry);
        }
        fclose(f);
        cJSON_AddItemToObject(json, "turns", turns);
        return json;
    }

    void PlayChatlogPath(const char* path, int channel_mode) {
        auto& app = Application::GetInstance();
        auto& audio = app.GetAudioService();
        auto codec = Board::GetInstance().GetAudioCodec();

        audio.Stop();
        vTaskDelay(pdMS_TO_TICKS(100));
        codec->EnableOutput(true);
        codec->EnableInput(false);

        FILE* f = fopen(path, "rb");
        if (!f) {
            ESP_LOGE(TAG, "Chatlog playback: cannot open %s errno=%d", path, errno);
            ShowNotify("Play Failed");
            ResumeAudioService();
            chatlog_playing_ = false;
            return;
        }
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 44, SEEK_SET);
        long data_size = (file_size > 44) ? (file_size - 44) : 0;
        const char* mode_name = (channel_mode == 1) ? "mic" : (channel_mode == 2) ? "ai" : "mixed";

        std::vector<int16_t> stereo(1024 * 2);
        std::vector<int16_t> mono(1024);
        size_t n;
        long played = 0;
        int last_pct = -1;
        ShowNotify("Playing... 0%");
        while (chatlog_playing_ && (n = fread(stereo.data(), 1, stereo.size() * sizeof(int16_t), f)) > 0) {
            size_t frames = n / (2 * sizeof(int16_t));
            for (size_t i = 0; i < frames; i++) {
                int16_t ch0 = stereo[i * 2];
                int16_t ch1 = stereo[i * 2 + 1];
                if (channel_mode == 1) {
                    mono[i] = ch0;
                } else if (channel_mode == 2) {
                    mono[i] = ch1;
                } else {
                    mono[i] = (int16_t)(((int)ch0 + (int)ch1) / 2);
                }
            }
            mono.resize(frames);
            codec->OutputData(mono);
            mono.resize(1024);
            played += n;
            int pct = (data_size > 0) ? (int)(played * 100 / data_size) : 100;
            if (pct >= last_pct + 25) {
                last_pct = (pct / 25) * 25;
                char prog[48];
                snprintf(prog, sizeof(prog), "Playing... %d%% (%s)", last_pct, mode_name);
                ShowNotify(prog, 3000);
            }
        }
        fclose(f);
        codec->EnableOutput(false);
        ResumeAudioService();
        chatlog_playing_ = false;
        ShowNotify("Play Done");
    }

    bool PlayChatlogByName(const char* filename, int channel_mode) {
        if (!InitializeSdCard()) return false;
        if (music_playing_ || chatlog_playing_) return false;
        char wav_path[256];
        BuildChatlogPath(wav_path, sizeof(wav_path), filename, "wav");
        struct stat st;
        if (stat(wav_path, &st) != 0) return false;
        static char play_path[256];
        strncpy(play_path, wav_path, sizeof(play_path) - 1);
        play_path[sizeof(play_path) - 1] = '\0';
        chatlog_playing_ = true;
        chatlog_stop_ = false;
        chatlog_channel_mode_ = channel_mode;
        xTaskCreatePinnedToCore([](void* arg) {
            auto* board = static_cast<BrookesiaAmoled2inch06*>(arg);
            board->PlayChatlogPath(play_path, board->chatlog_channel_mode_);
            vTaskDelete(NULL);
        }, "chatlog_play", 8 * 1024, this, 3, NULL, 1);
        return true;
    }

    bool DeleteChatlogByName(const char* filename) {
        if (!InitializeSdCard()) return false;
        char txt_path[256], wav_path[256];
        BuildChatlogPath(txt_path, sizeof(txt_path), filename, "txt");
        BuildChatlogPath(wav_path, sizeof(wav_path), filename, "wav");
        struct stat st;
        if (stat(txt_path, &st) != 0) return false;
        bool ok = (unlink(txt_path) == 0);
        unlink(wav_path);
        if (ok) {
            ESP_LOGI(TAG, "Deleted chatlog: %s (+wav)", txt_path);
            return true;
        }
        ESP_LOGE(TAG, "Chatlog delete failed %s errno=%d", txt_path, errno);
        return false;
    }

    cJSON* ListSystemLogsJson() {
        cJSON* json = cJSON_CreateObject();
        cJSON* files = cJSON_CreateArray();
        if (!InitializeSdCard()) {
            cJSON_AddStringToObject(json, "error", "no SD card");
            cJSON_AddItemToObject(json, "system_logs", files);
            return json;
        }
        DIR* dir = opendir("/sdcard/logs");
        if (!dir) {
            cJSON_AddStringToObject(json, "error", "no logs directory");
            cJSON_AddItemToObject(json, "system_logs", files);
            return json;
        }
        struct { char name[64]; long size; } list[40];
        int count = 0;
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL && count < 40) {
            bool is_syslog = (strncmp(ent->d_name, "log_", 4) == 0);
            bool is_blelog = (strncmp(ent->d_name, "ble_", 4) == 0);
            if ((is_syslog || is_blelog) && strstr(ent->d_name, ".txt")) {
                char path[300];
                snprintf(path, sizeof(path), "/sdcard/logs/%.*s", (int)(sizeof(path) - 14), ent->d_name);
                struct stat st;
                long size = 0;
                if (stat(path, &st) == 0) size = st.st_size;
                snprintf(list[count].name, sizeof(list[count].name), "%.*s",
                         (int)sizeof(list[count].name) - 1, ent->d_name);
                list[count].size = size;
                count++;
            }
        }
        closedir(dir);
        int max_show = count < 20 ? count : 20;
        for (int i = count - 1, j = 0; i >= 0 && j < max_show; i--, j++) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", list[i].name);
            cJSON_AddNumberToObject(item, "size", list[i].size);
            cJSON_AddItemToArray(files, item);
        }
        cJSON_AddStringToObject(json, "directory", "/sdcard/logs");
        cJSON_AddNumberToObject(json, "count", count);
        cJSON_AddItemToObject(json, "system_logs", files);
        return json;
    }

    void RunSelfTest() {
        if (self_test_running_) return;
        self_test_running_ = true;
        xTaskCreate([](void* arg) {
            auto* board = static_cast<BrookesiaAmoled2inch06*>(arg);
            board->RunSelfTestImpl();
            vTaskDelete(NULL);
        }, "self_test", 8 * 1024, this, 8, NULL);
    }

    void RunSelfTestImpl() {
        auto codec = Board::GetInstance().GetAudioCodec();
        cJSON* root = cJSON_CreateObject();
        cJSON* items = cJSON_CreateArray();

        auto add_item = [&items](const char* name, bool pass, const char* detail) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", name);
            cJSON_AddBoolToObject(item, "passed", pass);
            cJSON_AddStringToObject(item, "detail", detail);
            cJSON_AddItemToArray(items, item);
        };

        add_item("Display", true, "SH8601 initialized");

        int boot = gpio_get_level(BOOT_BUTTON_GPIO);
        add_item("Buttons", true, boot ? "BOOT=1" : "BOOT=0");

        char sd_detail[64] = "no SD";
        bool sd_ok = false;
        if (InitializeSdCard()) {
            FILE* probe = fopen("/sdcard/selftest_probe.bin", "wb");
            if (probe) {
                uint8_t wbuf[8] = {1,2,3,4,5,6,7,8};
                fwrite(wbuf, 1, sizeof(wbuf), probe);
                fclose(probe);
                probe = fopen("/sdcard/selftest_probe.bin", "rb");
                uint8_t rbuf[8] = {0};
                size_t rd = 0;
                if (probe) {
                    rd = fread(rbuf, 1, sizeof(rbuf), probe);
                    fclose(probe);
                }
                unlink("/sdcard/selftest_probe.bin");
                sd_ok = (rd == sizeof(wbuf) && memcmp(wbuf, rbuf, sizeof(wbuf)) == 0);
                snprintf(sd_detail, sizeof(sd_detail), sd_ok ? "RW ok" : "RW mismatch");
            }
        }
        add_item("SDCard", sd_ok, sd_detail);

        int level; bool charging, discharging;
        bool batt_ok = GetBatteryLevel(level, charging, discharging);
        char batt_detail[64];
        snprintf(batt_detail, sizeof(batt_detail), "%d%% chg=%d", level, charging);
        add_item("Battery", batt_ok, batt_detail);

        char audio_detail[64] = "skipped";
        bool audio_ok = false;
        if (codec) {
            auto& app = Application::GetInstance();
            auto& audio = app.GetAudioService();
            audio.Stop();
            vTaskDelay(pdMS_TO_TICKS(100));
            codec->EnableInput(true);
            codec->EnableOutput(false);
            std::vector<int16_t> pcm(512 * 2);
            bool cap = codec->InputData(pcm);
            codec->EnableInput(false);
            codec->EnableOutput(true);
            std::vector<int16_t> out(512);
            for (size_t i = 0; i < out.size(); i++) out[i] = pcm[i * 2];
            codec->OutputData(out);
            codec->EnableOutput(false);
            ResumeAudioService();
            audio_ok = cap;
            snprintf(audio_detail, sizeof(audio_detail), cap ? "cap+play ok" : "cap failed");
        }
        add_item("Audio", audio_ok, audio_detail);

        cJSON_AddItemToObject(root, "items", items);
        {
            std::lock_guard<std::mutex> lock(self_test_mutex_);
            self_test_result_ = cJSON_PrintUnformatted(root);
        }
        cJSON_Delete(root);
        self_test_running_ = false;
        ESP_LOGI(TAG, "SelfTest done: %s", self_test_result_.c_str());
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

        mcp.AddTool("self.get_battery_level",
            "Query the current battery level in percent and charging status.\n"
            "Use when the user asks about battery level, remaining power, or charging.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                int level; bool charging, discharging;
                if (GetBatteryLevel(level, charging, discharging)) {
                    cJSON* json = cJSON_CreateObject();
                    cJSON_AddNumberToObject(json, "level", level);
                    cJSON_AddBoolToObject(json, "charging", charging);
                    cJSON_AddBoolToObject(json, "discharging", discharging);
                    return static_cast<cJSON*>(json);
                }
                return std::string("Error: battery level not available");
            });

        mcp.AddTool("self.record_audio",
            "Record audio from the microphone to the SD card.\n"
            "duration_seconds: 1-120 (default 5). Returns a status message.",
            PropertyList({
                Property("duration_seconds", kPropertyTypeInteger, 5, 1, 120)
            }),
            [this](const PropertyList& props) -> ReturnValue {
                int duration = props["duration_seconds"].value<int>();
                if (StartRecording(duration)) {
                    return std::string("Recording started for ") + std::to_string(duration) + "s";
                }
                return std::string("Error: recording failed (SD card or already recording)");
            });

        mcp.AddTool("self.list_recordings",
            "List voice recordings on the SD card (/sdcard/records/).",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                return static_cast<cJSON*>(ListRecordingsJson());
            });

        mcp.AddTool("self.play_recording",
            "Play a voice recording by filename (from self.list_recordings).",
            PropertyList({
                Property("filename", kPropertyTypeString)
            }),
            [this](const PropertyList& props) -> ReturnValue {
                std::string filename = props["filename"].value<std::string>();
                if (PlayRecordingByName(filename.c_str())) {
                    return std::string("Playing recording: " + filename);
                }
                return std::string("Error: recording not found or SD unavailable: " + filename);
            });

        mcp.AddTool("self.delete_recording",
            "Delete a voice recording by filename.",
            PropertyList({
                Property("filename", kPropertyTypeString)
            }),
            [this](const PropertyList& props) -> ReturnValue {
                std::string filename = props["filename"].value<std::string>();
                if (DeleteRecordingByName(filename.c_str())) {
                    return std::string("Deleted recording: " + filename);
                }
                return std::string("Error: recording not found or delete failed: " + filename);
            });

        mcp.AddTool("self.list_music",
            "List music files on the SD card (/sdcard/music/).",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                return static_cast<cJSON*>(ListMusicJson());
            });

        mcp.AddTool("self.play_music",
            "Play an MP3/M4A/AAC music file from /sdcard/music/ (by filename from self.list_music).",
            PropertyList({
                Property("filename", kPropertyTypeString)
            }),
            [this](const PropertyList& props) -> ReturnValue {
                std::string filename = props["filename"].value<std::string>();
                if (music_playing_) return std::string("Error: music already playing. Stop it first.");
                if (PlayMusicByName(filename.c_str())) {
                    return std::string("Playing music: " + filename);
                }
                return std::string("Error: music file not found or SD unavailable: " + filename);
            });

        mcp.AddTool("self.stop_music",
            "Stop the currently playing music.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                StopMusic();
                return std::string("Music stop requested");
            });

        mcp.AddTool("self.delete_music",
            "Delete a music file by filename.",
            PropertyList({
                Property("filename", kPropertyTypeString)
            }),
            [this](const PropertyList& props) -> ReturnValue {
                std::string filename = props["filename"].value<std::string>();
                if (DeleteMusicByName(filename.c_str())) {
                    return std::string("Deleted music: " + filename);
                }
                return std::string("Error: music file not found or delete failed: " + filename);
            });

        mcp.AddTool("self.list_chatlogs",
            "List chat logs on the SD card. Optional directory: chatlogs (default) or system_logs.",
            PropertyList({
                Property("directory", kPropertyTypeString, std::string("chatlogs"))
            }),
            [this](const PropertyList& props) -> ReturnValue {
                std::string dir_param = props["directory"].value<std::string>();
                if (dir_param == "system_logs" || dir_param == "logs") {
                    return static_cast<cJSON*>(ListSystemLogsJson());
                }
                return static_cast<cJSON*>(ListChatlogsJson());
            });

        mcp.AddTool("self.get_chatlog_summary",
            "Get the text transcript of a specific chat conversation log.\n"
            "filename must be from self.list_chatlogs results.",
            PropertyList({
                Property("filename", kPropertyTypeString)
            }),
            [this](const PropertyList& props) -> ReturnValue {
                std::string filename = props["filename"].value<std::string>();
                return static_cast<cJSON*>(GetChatlogSummaryJson(filename.c_str()));
            });

        mcp.AddTool("self.play_chatlog_audio",
            "Play the audio recording of a specific chat conversation.\n"
            "channel: mixed (default), mic, or ai.",
            PropertyList({
                Property("filename", kPropertyTypeString),
                Property("channel", kPropertyTypeString, std::string("mixed"))
            }),
            [this](const PropertyList& props) -> ReturnValue {
                std::string filename = props["filename"].value<std::string>();
                std::string channel = props["channel"].value<std::string>();
                int mode = 0;
                if (channel == "mic") mode = 1;
                else if (channel == "ai") mode = 2;
                if (PlayChatlogByName(filename.c_str(), mode)) {
                    return std::string("Playing chatlog audio: " + filename);
                }
                return std::string("Error: chatlog wav not found or busy: " + filename);
            });

        mcp.AddTool("self.delete_chatlog",
            "Delete a chat conversation log (txt + wav) by filename.",
            PropertyList({
                Property("filename", kPropertyTypeString)
            }),
            [this](const PropertyList& props) -> ReturnValue {
                std::string filename = props["filename"].value<std::string>();
                if (DeleteChatlogByName(filename.c_str())) {
                    return std::string("Deleted chatlog: " + filename);
                }
                return std::string("Error: chatlog not found or delete failed: " + filename);
            });

        mcp.AddTool("self.run_self_test",
            "Run a hardware self-test (display, buttons, SD, battery, audio).\n"
            "Returns a summary of pass/fail results.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                if (self_test_running_) return std::string("Self-test already running");
                RunSelfTest();
                return std::string("Self-test started, results available shortly");
            });

        mcp.AddTool("self.get_self_test_result",
            "Get the last self-test result as JSON.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                std::lock_guard<std::mutex> lock(self_test_mutex_);
                if (self_test_result_.empty()) return std::string("No self-test result yet. Run self.run_self_test first.");
                return self_test_result_;
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

        auto& ssid_mgr = SsidManager::GetInstance();
        if (ssid_mgr.GetSsidList().empty()) {
            ssid_mgr.AddSsid("rickqi11", "18620907850");
        }

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
