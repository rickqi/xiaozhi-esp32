#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_log.h>
#include <string.h>
#include <esp_netif.h>
#include <mbedtls/base64.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "custom_lcd_display.h"
#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "system_info.h"
#include "assets/lang_config.h"
#include "codecs/box_audio_codec.h"
#include "wifi_station.h"
#include "mcp_server.h"
#include "lvgl.h"
#include "custom_lcd_display.h"

#define TAG "waveshare_rlcd_4_2"

// PCF85063 RTC registers (I2C addr 0x51)
#define PCF85063_ADDR       0x51
#define PCF85063_CTRL1      0x00
#define PCF85063_CTRL1_STOP (1 << 5)
#define PCF85063_SEC_REG    0x04
#define PCF85063_MIN_REG    0x05
#define PCF85063_HR_REG     0x06
#define PCF85063_DAY_REG    0x07
#define PCF85063_WEEK_REG   0x08
#define PCF85063_MONTH_REG  0x09
#define PCF85063_YEAR_REG   0x0A

// BCD helpers
static inline uint8_t RtcToBcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static inline int RtcFromBcd(uint8_t v) { return (int)(((v >> 4) * 10) + (v & 0x0F)); }

class CustomBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    // KEY button: short_press_time=500ms widens the multi-click window so a
    // natural triple-click (gaps up to ~500ms) is still recognized as 3 clicks.
    // The default 180ms (CONFIG_BUTTON_SHORT_PRESS_TIME_MS) is too tight:
    // a slow 3rd click breaks the sequence and only double-click fires.
    Button key_button_{GPIO_NUM_18, false, 0, 500};
    sdmmc_card_t *sdcard_card_ = nullptr;
    bool sdcard_mounted_ = false;
    volatile bool recording_ = false;
    volatile uint32_t recording_until_ms_ = 0;  // auto-stop time for timed recording
    FILE *log_file_ = nullptr;
    char log_path_[64] = "";
    CustomLcdDisplay *display_;
    adc_oneshot_unit_handle_t adc1_handle;
    adc_cali_handle_t cali_handle;
    bool vbat_status = 0;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = ESP32_I2C_HOST;
        i2c_bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    // ---- PCF85063 RTC minimal driver (uses new IDF I2C master API) ----
    bool RtcReadTime(struct tm *tm) {
        if (i2c_bus_ == nullptr) return false;
        i2c_master_dev_handle_t dev;
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = PCF85063_ADDR;
        dev_cfg.scl_speed_hz = 100000;
        if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &dev) != ESP_OK) return false;

        uint8_t reg = PCF85063_SEC_REG;
        uint8_t buf[7];
        esp_err_t ret = i2c_master_transmit_receive(dev, &reg, 1, buf, 7, 100);
        i2c_master_bus_rm_device(dev);
        if (ret != ESP_OK) return false;

        // Check oscillator stop flag (bit7 of seconds register)
        if (buf[0] & 0x80) {
            ESP_LOGW(TAG, "RTC: oscillator stop flag set, time may be invalid");
        }
        tm->tm_sec  = RtcFromBcd(buf[0] & 0x7F);
        tm->tm_min  = RtcFromBcd(buf[1] & 0x7F);
        tm->tm_hour = RtcFromBcd(buf[2] & 0x3F);
        tm->tm_mday = RtcFromBcd(buf[3] & 0x3F);
        tm->tm_wday = RtcFromBcd(buf[4] & 0x07);
        tm->tm_mon  = RtcFromBcd(buf[5] & 0x1F) - 1;
        tm->tm_year = RtcFromBcd(buf[6]) + 100;  // PCF85063 years 00-99 -> 2000-2099
        return true;
    }

    bool RtcWriteTime(const struct tm *tm) {
        if (i2c_bus_ == nullptr) return false;
        i2c_master_dev_handle_t dev;
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = PCF85063_ADDR;
        dev_cfg.scl_speed_hz = 100000;
        if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &dev) != ESP_OK) return false;

        // Stop clock
        uint8_t ctrl = PCF85063_CTRL1_STOP;
        uint8_t wbuf[2] = {PCF85063_CTRL1, ctrl};
        i2c_master_transmit(dev, wbuf, 2, 100);

        // Write time registers (reg addr + 7 time bytes)
        uint8_t data[8];
        data[0] = PCF85063_SEC_REG;
        data[1] = RtcToBcd(tm->tm_sec);
        data[2] = RtcToBcd(tm->tm_min);
        data[3] = RtcToBcd(tm->tm_hour);
        data[4] = RtcToBcd(tm->tm_mday);
        data[5] = RtcToBcd(tm->tm_wday);
        data[6] = RtcToBcd(tm->tm_mon + 1);
        data[7] = RtcToBcd((tm->tm_year - 100) & 0xFF);
        esp_err_t ret = i2c_master_transmit(dev, data, 8, 100);

        // Restart clock
        ctrl = 0x00;
        wbuf[1] = ctrl;
        i2c_master_transmit(dev, wbuf, 2, 100);
        i2c_master_bus_rm_device(dev);
        return (ret == ESP_OK);
    }

    // Read RTC at boot and set the system clock so time() works instantly
    void InitRtcClock() {
        struct tm tm;
        if (RtcReadTime(&tm)) {
            if (tm.tm_year >= 100 && tm.tm_year < 200) {  // valid 2000-2099
                struct timeval tv;
                tv.tv_sec = mktime(&tm);
                tv.tv_usec = 0;
                settimeofday(&tv, NULL);
                ESP_LOGI(TAG, "RTC: boot time %04d-%02d-%02d %02d:%02d:%02d",
                         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                         tm.tm_hour, tm.tm_min, tm.tm_sec);
            } else {
                ESP_LOGW(TAG, "RTC: invalid time, skipping");
            }
        } else {
            ESP_LOGW(TAG, "RTC: read failed");
        }
    }

    // Called after server/NTP time sync to keep RTC accurate
    void SyncRtcToSystemTimeImpl() {
        time_t now = time(NULL);
        if (now < 1600000000) return;  // before 2020, not synced yet
        struct tm tm;
        localtime_r(&now, &tm);
        if (RtcWriteTime(&tm)) {
            ESP_LOGI(TAG, "RTC: synced to %04d-%02d-%02d %02d:%02d:%02d",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
        }
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
        // Long press triggers screenshot
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "BOOT long press, taking screenshot");
            TakeScreenshot();
        });

        // KEY button (GPIO18)
        // Single click: toggle microphone mute
        key_button_.OnClick([this]() {
            auto codec = Board::GetInstance().GetAudioCodec();
            if (codec != nullptr) {
                bool muted = !codec->input_enabled();
                codec->EnableInput(!muted);
                ESP_LOGI(TAG, "KEY click: mic %s", muted ? "MUTED" : "UNMUTED");
                auto display = Board::GetInstance().GetDisplay();
                if (display) {
                    display->ShowNotification(muted ? "Mic Muted" : "Mic On");
                }
            }
        });
        // Double click: play popup test sound
        key_button_.OnDoubleClick([this]() {
            ESP_LOGI(TAG, "KEY double click: play test sound");
            auto& app = Application::GetInstance();
            app.PlaySound(Lang::Sounds::OGG_POPUP);
        });
        // Long press: show system info (IP/MAC/version)
        key_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "KEY long press: show system info");
            auto display = Board::GetInstance().GetDisplay();
            if (display == nullptr) return;

            // IP
            esp_netif_ip_info_t ip_info;
            std::string ip = "--";
            if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info) == ESP_OK) {
                uint32_t addr = ip_info.ip.addr;
                char buf[24];
                snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
                    (int)(addr & 0xFF), (int)((addr >> 8) & 0xFF),
                    (int)((addr >> 16) & 0xFF), (int)((addr >> 24) & 0xFF));
                ip = buf;
            }
            // MAC + version
            std::string mac = SystemInfo::GetMacAddress();
            std::string version = SystemInfo::GetUserAgent();
            std::string info = "IP: " + ip + "  MAC: " + mac + "  " + version;
            display->ShowNotification(info.c_str(), 6000);
        });

        // BOOT double click: toggle recording to SD card
        boot_button_.OnDoubleClick([this]() {
            ESP_LOGI(TAG, "BOOT double click: toggle recording");
            ToggleRecording();
        });

        // KEY triple click: play latest recording (spawn dedicated task to avoid blocking button context)
        key_button_.OnMultipleClick([this]() {
            ESP_LOGI(TAG, "KEY triple click: play latest recording");
            xTaskCreatePinnedToCore(PlayTask, "sd_play", 8 * 1024, this, 3, NULL, 1);
        }, 3);
    }

    static void PlayTask(void *arg) {
        auto board = (CustomBoard *)arg;
        board->PlayLatestRecording();
        vTaskDelete(NULL);
    }

    // ---- SD card (TF) support ----
    // GPIO18 KEY button raw-level monitor: prints every level change directly
    // to serial (independent of iot_button) to verify the button hardware works.
    static void KeyLevelMonitorTask(void *arg) {
        // Read current level as baseline
        int last = gpio_get_level(GPIO_NUM_18);
        printf("[KEYMON] GPIO18 initial level=%d\n", last);
        fflush(stdout);
        for (;;) {
            int cur = gpio_get_level(GPIO_NUM_18);
            if (cur != last) {
                int64_t t = esp_timer_get_time() / 1000;
                printf("[KEYMON] GPIO18 changed: %d -> %d at %lldms\n", last, cur, t);
                fflush(stdout);
                last = cur;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    bool InitializeSdCard() {
        if (sdcard_mounted_) return true;
        esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
        mount_config.format_if_mount_failed = false;
        mount_config.max_files = 5;
        mount_config.allocation_unit_size = 16 * 1024 * 3;
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_config.width = 1;
        slot_config.clk = SD_CARD_CLK_PIN;
        slot_config.cmd = SD_CARD_CMD_PIN;
        slot_config.d0 = SD_CARD_D0_PIN;
        esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &sdcard_card_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
            return false;
        }
        sdcard_mounted_ = true;
        sdmmc_card_print_info(stdout, sdcard_card_);
        // Ensure records and logs directories exist
        int ret_rec = mkdir("/sdcard/records", 0755);
        int ret_log = mkdir("/sdcard/logs", 0755);
        ESP_LOGI(TAG, "SD dirs: records=%d logs=%d", ret_rec, ret_log);
        ESP_LOGI(TAG, "SD card mounted");
        return true;
    }

    void OpenLogFile() {
        if (log_file_ != nullptr) return;
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        if (tm->tm_year < 100) {
            // Time not synced yet; use fallback timestamp
            snprintf(log_path_, sizeof(log_path_), "/sdcard/logs/log_00000000.txt");
        } else {
            snprintf(log_path_, sizeof(log_path_), "/sdcard/logs/log_%04d%02d%02d.txt",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
        }
        log_file_ = fopen(log_path_, "a");
        if (log_file_) {
            ESP_LOGI(TAG, "Log file: %s", log_path_);
        } else {
            ESP_LOGW(TAG, "Cannot open log file %s errno=%d", log_path_, errno);
        }
    }

    // Custom vprintf: write to original console AND log file on SD card
    static int SdLogVprintf(const char *fmt, va_list args) {
        auto board = GetSdLogBoard();
        // Format the message (va_copy: args must not be consumed before orig_vprintf_)
        char buf[256];
        va_list args_console;
        va_copy(args_console, args);
        int len = vsnprintf(buf, sizeof(buf), fmt, args);
        if (len < 0) len = 0;
        if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
        // Write to original console (UART + USB)
        if (board->orig_vprintf_) {
            board->orig_vprintf_(fmt, args_console);
        } else {
            vprintf(fmt, args_console);
        }
        va_end(args_console);
        // Write to SD log file (append, thread-safe via mutex)
        if (board->sdcard_mounted_ && board->log_file_ != nullptr) {
            board->log_mutex_.lock();
            size_t wr = fwrite(buf, 1, len, board->log_file_);
            int err = ferror(board->log_file_);
            // NOTE: fflush() only flushes stdio -> VFS, but without
            // CONFIG_FATFS_IMMEDIATE_FSYNC the data stays in the FATFS sector
            // cache and is NOT visible to other handles / not durable.
            // fsync() calls f_sync() which actually writes to the SD card.
            fflush(board->log_file_);
            int fr = fsync(fileno(board->log_file_));
            board->log_mutex_.unlock();
            if (wr != (size_t)len) {
                // One-shot warning, don't spam
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    printf("[SdLog] fwrite failed wr=%d err=%d fsync=%d\n", (int)wr, err, fr);
                }
            }
        }
        return len;
    }

    // Static accessor for the board instance in static callback
    static CustomBoard* GetSdLogBoard() { return sd_log_board_; }
    static CustomBoard* sd_log_board_;  // defined outside class
    vprintf_like_t orig_vprintf_ = nullptr;
    std::mutex log_mutex_;

    void InitializeSdLog() {
        sd_log_board_ = this;
        // Mount SD card immediately (works in constructor)
        if (!InitializeSdCard()) return;
        // Delay log file open + vprintf registration to after boot (FATFS VFS fully ready)
        xTaskCreatePinnedToCore(SdLogInitTask, "sd_log_init", 4 * 1024, this, 1, NULL, 1);
    }

    static void SdLogInitTask(void *arg) {
        auto board = (CustomBoard *)arg;
        vTaskDelay(pdMS_TO_TICKS(5000));  // wait for boot to settle
        board->OpenLogFile();
        // Self-test: write directly to verify FATFS write path works
        if (board->log_file_ != nullptr) {
            const char *test = "SDLOG_INIT_TEST ok\n";
            size_t wr = fwrite(test, 1, strlen(test), board->log_file_);
            fflush(board->log_file_);
            printf("[SdLog] self-test wrote %d bytes (no fsync yet)\n", (int)wr);
            // TEST A: read with an independent handle while write handle still open
            FILE *rf = fopen(board->log_path_, "r");
            if (rf) {
                char rb[64] = "";
                int got = (fgets(rb, sizeof(rb), rf) != NULL) ? (int)strlen(rb) : 0;
                printf("[SdLog] TEST A (no fsync): got %d bytes [%s]\n", got, rb);
                fclose(rf);
            } else {
                printf("[SdLog] TEST A open failed errno=%d\n", errno);
            }
            // TEST C: fsync then read
            int fr = fsync(fileno(board->log_file_));
            printf("[SdLog] fsync() = %d errno=%d\n", fr, errno);
            FILE *rf3 = fopen(board->log_path_, "r");
            if (rf3) {
                char rb[64] = "";
                int got = (fgets(rb, sizeof(rb), rf3) != NULL) ? (int)strlen(rb) : 0;
                printf("[SdLog] TEST C (after fsync): got %d bytes [%s]\n", got, rb);
                fclose(rf3);
            } else {
                printf("[SdLog] TEST C open failed errno=%d\n", errno);
            }
        } else {
            printf("[SdLog] log_file_ is NULL\n");
        }
        // Install custom vprintf to tee logs to SD
        board->orig_vprintf_ = esp_log_set_vprintf(SdLogVprintf);
        ESP_LOGI(TAG, "SD log tee enabled");
        vTaskDelete(NULL);
    }

    // ---- Recording to SD card (WAV format) ----
    void ToggleRecording() {
        if (recording_) {
            recording_ = false;  // signal task to stop
            return;
        }
        if (!InitializeSdCard()) {
            ShowNotify("No SD Card");
            return;
        }
        recording_ = true;
        xTaskCreatePinnedToCore(RecordTask, "sd_record", 8 * 1024, this, 3, NULL, 1);
    }

    static void RecordTask(void *arg) {
        auto board = (CustomBoard *)arg;
        board->RecordTaskImpl();
        vTaskDelete(NULL);
    }

    void RecordTaskImpl() {
        auto& app = Application::GetInstance();
        auto& audio = app.GetAudioService();
        auto codec = Board::GetInstance().GetAudioCodec();

        // Pause AI audio service entirely to free the I2S bus and codec
        audio.Stop();
        vTaskDelay(pdMS_TO_TICKS(100));  // wait for audio tasks to exit before using codec
        codec->EnableInput(true);
        codec->EnableOutput(false);

        // Generate filename: /sdcard/records/rec_YYYYMMDD_HHMMSS.wav
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char path[64];
        snprintf(path, sizeof(path), "/sdcard/records/rec_%04d%02d%02d_%02d%02d%02d.wav",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec);

        // WAV header: 24kHz, 2ch, 16bit
        const int sample_rate = 24000;
        const int channels = 2;
        const int bits = 16;
        uint32_t data_len = 0;
        FILE *f = fopen(path, "wb");
        if (!f) {
            ESP_LOGE(TAG, "Cannot create recording file");
            recording_ = false;
            ShowNotify("Rec Failed");
            // Restart AI audio
            audio.Start();
            return;
        }        // Write placeholder WAV header (44 bytes)
        uint8_t hdr[44] = {0};
        memcpy(hdr, "RIFF", 4);
        memcpy(hdr + 8, "WAVE", 4);
        memcpy(hdr + 12, "fmt ", 4);
        hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;  // fmt chunk size
        hdr[20] = 1; hdr[21] = 0;  // PCM
        hdr[22] = channels & 0xFF; hdr[23] = (channels >> 8) & 0xFF;
        hdr[24] = sample_rate & 0xFF; hdr[25] = (sample_rate >> 8) & 0xFF;
        hdr[26] = (sample_rate >> 16) & 0xFF; hdr[27] = (sample_rate >> 24) & 0xFF;
        uint32_t byte_rate = sample_rate * channels * bits / 8;
        hdr[28] = byte_rate & 0xFF; hdr[29] = (byte_rate >> 8) & 0xFF;
        hdr[30] = (byte_rate >> 16) & 0xFF; hdr[31] = (byte_rate >> 24) & 0xFF;
        hdr[32] = channels * bits / 8;  // block align
        hdr[34] = bits;                 // bits per sample
        memcpy(hdr + 36, "data", 4);
        fwrite(hdr, 1, 44, f);

        // Show recording start with audio parameters (keep visible 5s)
        char start_info[64];
        snprintf(start_info, sizeof(start_info), "REC %dkHz %dch %dbit %luKbps",
                 sample_rate / 1000, channels, bits,
                 (unsigned long)(sample_rate * channels * bits / 1000));
        ShowNotify(start_info, 5000);
        ESP_LOGI(TAG, "Recording to %s", path);

        // Stream-record: read codec PCM -> write to SD
        const int chunk_samples = 512;  // ~21ms at 24kHz
        std::vector<int16_t> pcm(chunk_samples * channels);
        uint32_t last_report_ms = 0;
        uint32_t start_ms = esp_timer_get_time() / 1000;
        while (recording_) {
            // Auto-stop for timed recording
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
            // Update recording progress notification every second
            if (now_ms - last_report_ms >= 1000) {
                last_report_ms = now_ms;
                uint32_t secs = (now_ms - start_ms) / 1000;
                uint32_t kb = data_len / 1024;
                // rate = bytes/s
                uint32_t rate_kbps = (data_len * 8 / 1000) / (secs > 0 ? secs : 1);
                char prog[64];
                snprintf(prog, sizeof(prog), "REC %lus %luKB %luKbps", (unsigned long)secs, (unsigned long)kb, (unsigned long)rate_kbps);
                ShowNotify(prog, 5000);  // keep visible while recording
            }
        }
        recording_until_ms_ = 0;

        // Finalize WAV header
        fseek(f, 4, SEEK_SET);
        uint32_t riff_size = 36 + data_len;
        fwrite(&riff_size, 1, 4, f);
        fseek(f, 40, SEEK_SET);
        fwrite(&data_len, 1, 4, f);
        fclose(f);

        // Restore AI audio service
        codec->EnableInput(false);
        audio.Start();

        float dur = (float)data_len / (sample_rate * channels * 2);
        char info[64];
        snprintf(info, sizeof(info), "Saved %u.%02us %luKB %s",
                 (int)dur, (int)(dur * 100) % 100,
                 (unsigned long)(data_len / 1024), strrchr(path, '/') + 1);
        ShowNotify(info, 4000);
        ESP_LOGI(TAG, "Recording done: %s (%u bytes, %.1fs)", path, (unsigned int)data_len, dur);
    }

    // ---- Playback latest recording from SD ----
    void PlayLatestRecording() {
        if (!InitializeSdCard()) {
            ShowNotify("No SD Card");
            return;
        }
        // Find latest rec_*.wav file
        DIR *dir = opendir("/sdcard/records");
        if (!dir) {
            ShowNotify("No Recordings");
            return;
        }
        char latest[256] = "";
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strncmp(ent->d_name, "rec_", 4) == 0 && strstr(ent->d_name, ".wav")) {
                strncpy(latest, "/sdcard/records/", sizeof(latest) - 1);
                latest[sizeof(latest) - 1] = '\0';
                strncat(latest, ent->d_name, sizeof(latest) - strlen(latest) - 1);
            }
        }
        closedir(dir);
        if (latest[0] == '\0') {
            ShowNotify("No Recordings");
            return;
        }

        PlayRecordingPath(latest);
    }

    // Play a specific WAV recording file (2ch -> 1ch downmix)
    void PlayRecordingPath(const char *path) {
        auto& app = Application::GetInstance();
        auto& audio = app.GetAudioService();
        auto codec = Board::GetInstance().GetAudioCodec();

        // Pause AI audio service to free the I2S bus and codec
        audio.Stop();
        vTaskDelay(pdMS_TO_TICKS(100));  // wait for audio tasks to exit before using codec
        codec->EnableOutput(true);
        codec->EnableInput(false);

        FILE *f = fopen(path, "rb");
        if (!f) {
            ESP_LOGE(TAG, "Playback: cannot open %s errno=%d", path, errno);
            ShowNotify("Play Failed");
            audio.Start();
            return;
        }
        // Get file size for progress
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 44, SEEK_SET);  // skip WAV header
        long data_size = (file_size > 44) ? (file_size - 44) : 0;
        ESP_LOGI(TAG, "Playback start: %s (%ld bytes, %.1fs)", path, file_size, (float)data_size / (24000 * 2 * 2));

        // Recording is 2ch (mic+ref), output is 1ch mono: extract mic channel (even samples)
        // Use larger buffer (1024 frames = 42ms @24kHz) for smoother playback
        std::vector<int16_t> stereo(1024 * 2);
        std::vector<int16_t> mono(1024);
        size_t n;
        long played = 0;
        int last_pct = -1;
        ShowNotify("Playing... 0%");
        while ((n = fread(stereo.data(), 1, stereo.size() * sizeof(int16_t), f)) > 0) {
            size_t frames = n / (2 * sizeof(int16_t));  // number of stereo frames
            for (size_t i = 0; i < frames; i++) {
                mono[i] = stereo[i * 2];  // extract channel 0 (mic)
            }
            mono.resize(frames);
            codec->OutputData(mono);  // OutputData takes vector<int16_t>&
            mono.resize(1024);
            played += n;
            // Update progress every ~25%
            int pct = (data_size > 0) ? (int)(played * 100 / data_size) : 100;
            if (pct >= last_pct + 25) {
                last_pct = (pct / 25) * 25;
                char prog[32];
                snprintf(prog, sizeof(prog), "Playing... %d%%", last_pct);
                ShowNotify(prog, 3000);
                ESP_LOGI(TAG, "Playback progress: %d%%", last_pct);
            }
        }
        fclose(f);
        ESP_LOGI(TAG, "Playback done: %s (%ld bytes played)", path, played);

        codec->EnableOutput(false);
        audio.Start();
        ShowNotify("Play Done");
    }

    void ShowNotify(const char *msg) {
        ShowNotify(msg, 3000);
    }
    void ShowNotify(const char *msg, int duration_ms) {
        auto display = Board::GetInstance().GetDisplay();
        if (display) display->ShowNotification(msg, duration_ms);
    }

    // List all recording files in /sdcard/records to serial
    void ListRecordings() {
        if (!InitializeSdCard()) {
            printf("RECORDINGS: no SD card\n");
            return;
        }
        DIR *dir = opendir("/sdcard/records");
        if (!dir) {
            printf("RECORDINGS: no records dir\n");
            return;
        }
        int count = 0;
        struct dirent *ent;
        printf("RECORDINGS_START\n");
        while ((ent = readdir(dir)) != NULL) {
            if (strncmp(ent->d_name, "rec_", 4) == 0 && strstr(ent->d_name, ".wav")) {
                // Get file size
                char path[256];
                strncpy(path, "/sdcard/records/", sizeof(path) - 1);
                path[sizeof(path) - 1] = '\0';
                strncat(path, ent->d_name, sizeof(path) - strlen(path) - 1);
                struct stat st;
                long size = 0;
                if (stat(path, &st) == 0) size = st.st_size;
                // Duration: 2ch 16bit 24kHz -> bytes / (24000*2*2)
                float dur = (size > 44) ? (float)(size - 44) / (24000 * 2 * 2) : 0.0f;
                printf("%s %ldB %.1fs\n", ent->d_name, size, dur);
                count++;
            }
        }
        closedir(dir);
        printf("RECORDINGS_END (%d files)\n", count);
        fflush(stdout);
    }

    // Build JSON list of recordings (newest first, max 10) for MCP tool
    cJSON *ListRecordingsJson() {
        cJSON *json = cJSON_CreateObject();
        cJSON *files = cJSON_CreateArray();
        if (!InitializeSdCard()) {
            cJSON_AddStringToObject(json, "error", "no SD card");
            cJSON_AddItemToObject(json, "recordings", files);
            return json;
        }
        DIR *dir = opendir("/sdcard/records");
        if (!dir) {
            cJSON_AddStringToObject(json, "error", "no records directory");
            cJSON_AddItemToObject(json, "recordings", files);
            return json;
        }
        // Collect all rec_*.wav entries
        struct {
            char name[128];
            long size;
            float duration;
        } list[20];
        int count = 0;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && count < 20) {
            if (strncmp(ent->d_name, "rec_", 4) == 0 && strstr(ent->d_name, ".wav")) {
                char path[256];
                strncpy(path, "/sdcard/records/", sizeof(path) - 1);
                path[sizeof(path) - 1] = '\0';
                strncat(path, ent->d_name, sizeof(path) - strlen(path) - 1);
                struct stat st;
                long size = 0;
                if (stat(path, &st) == 0) size = st.st_size;
                float dur = (size > 44) ? (float)(size - 44) / (24000 * 2 * 2) : 0.0f;
                snprintf(list[count].name, sizeof(list[count].name), "%.*s", (int)sizeof(list[count].name) - 1, ent->d_name);
                list[count].size = size;
                list[count].duration = dur;
                count++;
            }
        }
        closedir(dir);
        // Newest first (rec_YYYYMMDD_HHMMSS names sort chronologically) -> reverse
        int max_show = count < 10 ? count : 10;
        for (int i = count - 1, j = 0; i >= 0 && j < max_show; i--, j++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", list[i].name);
            cJSON_AddNumberToObject(item, "size", list[i].size);
            cJSON_AddNumberToObject(item, "duration_seconds", list[i].duration);
            cJSON_AddItemToArray(files, item);
        }
        cJSON_AddNumberToObject(json, "count", count);
        cJSON_AddItemToObject(json, "recordings", files);
        return json;
    }

    // Play a recording by filename (e.g. "rec_20260801_093022.wav")
    bool PlayRecordingByName(const char *filename) {
        if (!InitializeSdCard()) return false;
        char path[256];
        strncpy(path, "/sdcard/records/", sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        strncat(path, filename, sizeof(path) - strlen(path) - 1);
        // Verify file exists
        struct stat st;
        if (stat(path, &st) != 0) return false;
        // Spawn task to play (don't block MCP callback)
        static char play_path[256];
        strncpy(play_path, path, sizeof(play_path) - 1);
        play_path[sizeof(play_path) - 1] = '\0';
        xTaskCreatePinnedToCore([](void *arg) {
            auto board = (CustomBoard *)arg;
            board->PlayRecordingPath(play_path);
            vTaskDelete(NULL);
        }, "mcp_play", 8 * 1024, this, 3, NULL, 1);
        return true;
    }

    // Start a timed recording (duration_sec), auto-stops when done. Returns new filename.
    bool StartRecording(int duration_sec) {
        if (recording_) {
            // Already recording - ignore
            return false;
        }
        if (!InitializeSdCard()) return false;
        if (duration_sec <= 0) duration_sec = 5;
        if (duration_sec > 120) duration_sec = 120;  // cap at 2 min
        recording_until_ms_ = (esp_timer_get_time() / 1000) + duration_sec * 1000;
        recording_ = true;
        xTaskCreatePinnedToCore(RecordTask, "sd_record", 8 * 1024, this, 3, NULL, 1);
        return true;
    }

    // Delete a recording file by name. Returns true on success.
    bool DeleteRecordingByName(const char *filename) {
        if (!InitializeSdCard()) return false;
        char path[256];
        strncpy(path, "/sdcard/records/", sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        strncat(path, filename, sizeof(path) - strlen(path) - 1);
        struct stat st;
        if (stat(path, &st) != 0) return false;
        int ret = unlink(path);
        if (ret == 0) {
            ESP_LOGI(TAG, "Deleted recording: %s", path);
            return true;
        }
        ESP_LOGE(TAG, "Delete failed %s errno=%d", path, errno);
        return false;
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.disp.network", "重新配网", PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            EnterWifiConfigMode();
            return true;
        });

        // Temperature/humidity query (SHTC3 sensor)
        mcp_server.AddTool("self.get_temperature_humidity",
            "Query the current temperature in Celsius and relative humidity in percent "
            "from the onboard temperature/humidity sensor (SHTC3).\n"
            "Use this tool when the user asks about temperature, humidity, or environment conditions.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                float temp, hum;
                if (GetTemperatureHumidity(temp, hum)) {
                    cJSON* json = cJSON_CreateObject();
                    cJSON_AddNumberToObject(json, "temperature_celsius", temp);
                    cJSON_AddNumberToObject(json, "humidity_percent", hum);
                    return static_cast<cJSON*>(json);
                }
                return std::string("Error: temperature/humidity sensor not available");
            });

        // Battery level query
        mcp_server.AddTool("self.get_battery_level",
            "Query the current battery level in percent.\n"
            "Use this tool when the user asks about battery level, remaining power, or charging status.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                int level; bool charging, discharging;
                if (GetBatteryLevel(level, charging, discharging)) {
                    cJSON* json = cJSON_CreateObject();
                    cJSON_AddNumberToObject(json, "level", level);
                    cJSON_AddBoolToObject(json, "charging", charging);
                    return static_cast<cJSON*>(json);
                }
                return std::string("Error: battery level not available");
            });

        // List recording files (newest 10) from SD card
        mcp_server.AddTool("self.list_recordings",
            "List the most recent audio recording files stored on the SD card.\n"
            "Returns up to 10 recordings with their filename, size, and duration in seconds.\n"
            "Use this tool when the user asks what recordings are available, or to see saved recordings.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                return static_cast<cJSON*>(ListRecordingsJson());
            });

        // Play a specific recording file
        mcp_server.AddTool("self.play_recording",
            "Play a specific audio recording file from the SD card.\n"
            "The 'filename' parameter must be a filename from self.list_recordings results (e.g. rec_20260801_093022.wav).\n"
            "Use this tool when the user asks to play a specific recording.",
            PropertyList({
                Property("filename", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string filename = properties["filename"].value<std::string>();
                if (PlayRecordingByName(filename.c_str())) {
                    return std::string("Playing recording: " + filename);
                }
                return std::string("Error: recording file not found: " + filename);
            });

        // Record audio to SD card (timed)
        mcp_server.AddTool("self.record_audio",
            "Record audio from the microphone and save it to the SD card.\n"
            "The 'duration_seconds' parameter specifies how many seconds to record (1-120, default 5).\n"
            "Recording auto-stops after the duration. Returns the saved filename.\n"
            "Use this tool when the user asks to record audio, take a voice memo, or save a message.",
            PropertyList({
                Property("duration_seconds", kPropertyTypeInteger, 5, 1, 120)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int dur = properties["duration_seconds"].value<int>();
                if (StartRecording(dur)) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Recording %d seconds", dur);
                    return std::string(msg);
                }
                return std::string("Error: already recording or SD card not available");
            });

        // Delete a recording file
        mcp_server.AddTool("self.delete_recording",
            "Delete a specific recording file from the SD card.\n"
            "The 'filename' parameter must be a filename from self.list_recordings results (e.g. rec_20260801_093022.wav).\n"
            "Use this tool when the user asks to delete or remove a recording.",
            PropertyList({
                Property("filename", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string filename = properties["filename"].value<std::string>();
                if (DeleteRecordingByName(filename.c_str())) {
                    return std::string("Deleted recording: " + filename);
                }
                return std::string("Error: recording file not found or delete failed: " + filename);
            });
    }

    void InitializeLcdDisplay() {
        spi_display_config_t spi_config = {};
        spi_config.mosi = RLCD_MOSI_PIN;
        spi_config.scl = RLCD_SCK_PIN;
        spi_config.dc = RLCD_DC_PIN;
        spi_config.cs = RLCD_CS_PIN;
        spi_config.rst = RLCD_RST_PIN;
        display_ = new CustomLcdDisplay(NULL, NULL, RLCD_WIDTH,RLCD_HEIGHT,DISPLAY_OFFSET_X,DISPLAY_OFFSET_Y,DISPLAY_MIRROR_X,DISPLAY_MIRROR_Y,DISPLAY_SWAP_XY,spi_config);
    }

    void InitializeAdc() {
        adc_oneshot_unit_init_cfg_t init_config = {};
        init_config.unit_id = ADC_UNIT_1;
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc1_handle));
        adc_oneshot_chan_cfg_t chan_config = {};
        chan_config.atten = ADC_ATTEN_DB_12;
        chan_config.bitwidth = ADC_BITWIDTH_12;
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &chan_config));
        adc_cali_curve_fitting_config_t cali_config = {};
        cali_config.unit_id = ADC_UNIT_1;
        cali_config.atten = ADC_ATTEN_DB_12;
        cali_config.bitwidth = ADC_BITWIDTH_12;
        ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle));
    }

    // SHTC3 temperature/humidity sensor (I2C addr 0x70)
    bool ReadShtc3(float& temp_c, float& humidity_pct) {
        if (i2c_bus_ == nullptr) return false;
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = 0x70;
        dev_cfg.scl_speed_hz = 100000;
        i2c_master_dev_handle_t dev_handle;
        if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &dev_handle) != ESP_OK)
            return false;
        uint8_t wake_cmd[] = {0x35, 0x17};
        i2c_master_transmit(dev_handle, wake_cmd, 2, -1);
        vTaskDelay(pdMS_TO_TICKS(1));
        uint8_t meas_cmd[] = {0x7C, 0xA2};
        i2c_master_transmit(dev_handle, meas_cmd, 2, -1);
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t raw[6];
        esp_err_t ret = i2c_master_receive(dev_handle, raw, 6, -1);
        i2c_master_bus_rm_device(dev_handle);
        if (ret != ESP_OK) return false;
        uint16_t t_raw = ((uint16_t)raw[0] << 8) | raw[1];
        uint16_t h_raw = ((uint16_t)raw[3] << 8) | raw[4];
        // SHTC3 calibration: add -4C offset (SHTC3_PETP_VOL) and use 65536 normalization
        // Reference: 05_I2C_SHTC3 example T = 175*raw/65536 - 45 - 4
        temp_c = -45.0f + 175.0f * t_raw / 65536.0f - 4.0f;
        humidity_pct = 100.0f * h_raw / 65536.0f;
        ESP_LOGI(TAG, "SHTC3: T=%.1fC H=%.0f%% raw=%04x %04x", temp_c, humidity_pct, t_raw, h_raw);
        return true;
    }

    // Screenshot: read display buffer -> P4 PBM -> base64 -> serial
    // Protocol matches tools/screenshot.py:
    //   "SCREENSHOT_START\n" <base64 72 chars/line> "SCREENSHOT_END\n"
    void TakeScreenshot() {
        if (display_ == nullptr) return;
        int w = display_->GetWidth();
        int h = display_->GetHeight();
        int row_bytes = (w + 7) / 8;
        int hdr_len = snprintf(NULL, 0, "P4\n%d %d\n", w, h);
        int pbm_size = hdr_len + row_bytes * h;
        uint8_t *pbm = (uint8_t *)heap_caps_malloc(pbm_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!pbm) {
            printf("SCREENSHOT_ERROR: out of memory\n");
            return;
        }
        snprintf((char *)pbm, hdr_len + 1, "P4\n%d %d\n", w, h);
        uint8_t *pdata = pbm + hdr_len;
        for (int y = 0; y < h; y++) {
            for (int bx = 0; bx < row_bytes; bx++) {
                uint8_t byte = 0;
                for (int b = 0; b < 8; b++) {
                    int x = bx * 8 + b;
                    if (x >= w) break;
                    if (display_->GetPixel(x, y) == ColorBlack)
                        byte |= (0x80 >> b);
                }
                *pdata++ = byte;
            }
        }
        size_t b64_len = 0;
        mbedtls_base64_encode(NULL, 0, &b64_len, pbm, pbm_size);
        uint8_t *b64 = (uint8_t *)malloc(b64_len + 1);
        if (!b64) {
            free(pbm);
            printf("SCREENSHOT_ERROR: base64 alloc\n");
            return;
        }
        mbedtls_base64_encode(b64, b64_len, &b64_len, pbm, pbm_size);
        b64[b64_len] = '\0';
        printf("SCREENSHOT_START\n");
        const int chunk = 72;
        for (size_t i = 0; i < b64_len; i += chunk) {
            int remain = (int)b64_len - (int)i;
            int len = (remain < chunk) ? remain : chunk;
            printf("%.*s\n", len, (char *)b64 + i);
        }
        printf("SCREENSHOT_END\n");
        fflush(stdout);
        free(b64);
        free(pbm);
        ESP_LOGI(TAG, "Screenshot captured (%dx%d)", w, h);

        // Show bottom notification with screenshot info
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            char info[64];
            snprintf(info, sizeof(info), "Shot: %dx%d PBM:%dKB b64:%dKB",
                     w, h, pbm_size / 1024, (int)(b64_len / 1024));
            display->ShowNotification(info, 4000);
        }
    }

    // Serial command listener: waits for "SHOOT" on stdin
    static void ScreenshotCmdTask(void *arg) {
        auto board = (CustomBoard *)arg;
        char line[64];
        for (;;) {
            if (fgets(line, sizeof(line), stdin) != NULL) {
                line[strcspn(line, "\r\n")] = '\0';
                if (strcmp(line, "SHOOT") == 0) {
                    ESP_LOGI(TAG, "Screenshot requested via serial");
                    board->TakeScreenshot();
                } else if (strcmp(line, "LIST") == 0) {
                    ESP_LOGI(TAG, "List recordings requested via serial");
                    board->ListRecordings();
                } else if (strcmp(line, "LOG") == 0) {
                    ESP_LOGI(TAG, "Dump SD log requested via serial");
                    board->DumpSdLog();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // Dump the current SD log file contents over serial (for diagnostics)
    void DumpSdLog() {
        if (!sdcard_mounted_ || log_path_[0] == '\0') {
            printf("SDLOG: no log file (SD not mounted?)\n");
            fflush(stdout);
            return;
        }
        FILE *f = fopen(log_path_, "r");
        if (!f) {
            printf("SDLOG: cannot open %s errno=%d\n", log_path_, errno);
            fflush(stdout);
            return;
        }
        printf("SDLOG_START %s\n", log_path_);
        char line[256];
        while (fgets(line, sizeof(line), f) != NULL) {
            printf("%s", line);
        }
        fclose(f);
        printf("SDLOG_END\n");
        fflush(stdout);
    }

    // One-shot auto screenshot ~12s after boot (screen settled), then dump SD log for diagnostics
    static void AutoScreenshotTask(void *arg) {
        auto board = (CustomBoard *)arg;
        vTaskDelay(pdMS_TO_TICKS(12000));
        ESP_LOGI(TAG, "Auto screenshot after boot");
        board->TakeScreenshot();
        // Dump SD log to serial so recent button events are visible on COM4
        vTaskDelay(pdMS_TO_TICKS(500));
        board->DumpSdLog();
        vTaskDelete(NULL);
    }

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitRtcClock();          // read PCF85063 RTC -> settimeofday() for instant clock
        InitializeButtons();
        InitializeTools();
        InitializeLcdDisplay();
        InitializeAdc();
        InitializeSdLog();      // mount SD + tee logs to /sdcard/logs/
        xTaskCreatePinnedToCore(ScreenshotCmdTask, "scr_cmd", 6 * 1024, this, 1, NULL, 1);
        xTaskCreatePinnedToCore(AutoScreenshotTask, "scr_auto", 6 * 1024, this, 1, NULL, 1);
        xTaskCreatePinnedToCore(KeyLevelMonitorTask, "key_mon", 2 * 1024, this, 1, NULL, 1);
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

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual i2c_master_bus_handle_t GetI2cBus() override {
        return i2c_bus_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        int raw;
        esp_err_t err = adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &raw);
        if (err != ESP_OK) return false;
        int mv;
        adc_cali_raw_to_voltage(cali_handle, raw, &mv);
        float vol = mv * 3.0f / 1000.0f;
        charging = false;
        discharging = true;
        if (vol < 3.0f)       level = 0;
        else if (vol > 4.12f) level = 100;
        else                  level = (int)((vol - 3.0f) / 1.12f * 100);
        return true;
    }

    virtual bool GetTemperatureHumidity(float& temp, float& humidity) override {
        return ReadShtc3(temp, humidity);
    }

    virtual void SyncRtcToSystemTime() override {
        SyncRtcToSystemTimeImpl();
    }
};

// Global static for the SD log callback
CustomBoard* CustomBoard::sd_log_board_ = nullptr;

DECLARE_BOARD(CustomBoard);