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
#include "version_info.h"
#include "http_file_server.h"
#include "lvgl.h"
// ESP Audio Codec (espressif/esp_audio_codec) - MP3/AAC/M4A decoding
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"
#include "decoder/impl/esp_aac_dec.h"

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
    // Cached I2C device handles: created once and reused instead of
    // add_device/rm_device on every RTC/SHTC3 access.
    i2c_master_dev_handle_t rtc_dev_ = nullptr;
    i2c_master_dev_handle_t shtc3_dev_ = nullptr;
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
    bool self_test_running_ = false;
    std::string self_test_result_;  // last self-test result (JSON)
    std::mutex self_test_mutex_;    // guards self_test_result_ (task writer vs MCP reader)
    int64_t last_fsync_ms_ = 0;  // last fsync timestamp for SD log throttling
    // Music playback state (MP3 from /sdcard/music)
    volatile bool music_playing_ = false;
    volatile bool music_stop_ = false;
    // ChatLog playback state (chatlog WAV from /sdcard/logs/chatlogs)
    volatile bool chatlog_playing_ = false;
    volatile bool chatlog_stop_ = false;
    int chatlog_channel_mode_ = 0;  // 0=mixed, 1=mic, 2=ai (set before spawning play task)

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
    // Lazily create (and cache) an I2C device handle for the given address.
    // Avoids add_device/rm_device churn on every access.
    bool GetI2cDevice(uint8_t addr, i2c_master_dev_handle_t *dev) {
        if (i2c_bus_ == nullptr || dev == nullptr) return false;
        if (*dev != nullptr) return true;
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = addr;
        dev_cfg.scl_speed_hz = 100000;
        if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, dev) != ESP_OK) {
            *dev = nullptr;
            return false;
        }
        return true;
    }

    bool RtcReadTime(struct tm *tm) {
        if (!GetI2cDevice(PCF85063_ADDR, &rtc_dev_)) return false;

        uint8_t reg = PCF85063_SEC_REG;
        uint8_t buf[7];
        esp_err_t ret = i2c_master_transmit_receive(rtc_dev_, &reg, 1, buf, 7, 100);
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
        if (!GetI2cDevice(PCF85063_ADDR, &rtc_dev_)) return false;

        // Stop clock
        uint8_t ctrl = PCF85063_CTRL1_STOP;
        uint8_t wbuf[2] = {PCF85063_CTRL1, ctrl};
        i2c_master_transmit(rtc_dev_, wbuf, 2, 100);

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
        esp_err_t ret = i2c_master_transmit(rtc_dev_, data, 8, 100);

        // Restart clock
        ctrl = 0x00;
        wbuf[1] = ctrl;
        i2c_master_transmit(rtc_dev_, wbuf, 2, 100);
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
    // Debug-only helper: gated off by default (10ms polling forever wastes CPU).
    // Enable with: #define RLCD_ENABLE_KEY_LEVEL_MONITOR 1
#if defined(RLCD_ENABLE_KEY_LEVEL_MONITOR) && RLCD_ENABLE_KEY_LEVEL_MONITOR
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
#endif  // RLCD_ENABLE_KEY_LEVEL_MONITOR

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
        // Ensure records, logs and music directories exist
        int ret_rec = mkdir("/sdcard/records", 0755);
        int ret_log = mkdir("/sdcard/logs", 0755);
        int ret_music = mkdir("/sdcard/music", 0755);
        ESP_LOGI(TAG, "SD dirs: records=%d logs=%d music=%d", ret_rec, ret_log, ret_music);
        ESP_LOGI(TAG, "SD card mounted");
        return true;
    }

    void OpenLogFile() {
        if (log_file_ != nullptr) return;
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        if (tm.tm_year < 100) {
            // Time not synced yet; use fallback timestamp
            snprintf(log_path_, sizeof(log_path_), "/sdcard/logs/log_00000000.txt");
        } else {
            snprintf(log_path_, sizeof(log_path_), "/sdcard/logs/log_%04d%02d%02d.txt",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
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
            // Throttle fsync to at most once per 1s: per-line fsync blocks the
            // whole log path on slow SD cards and stalls every ESP_LOG* caller.
            fflush(board->log_file_);
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - board->last_fsync_ms_ >= 1000) {
                fsync(fileno(board->log_file_));
                board->last_fsync_ms_ = now_ms;
            }
            board->log_mutex_.unlock();
            if (wr != (size_t)len) {
                // One-shot warning, don't spam
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    printf("[SdLog] fwrite failed wr=%d err=%d\n", (int)wr, err);
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
        struct tm tm;
        localtime_r(&now, &tm);
        char path[64];
        snprintf(path, sizeof(path), "/sdcard/records/rec_%04d%02d%02d_%02d%02d%02d.wav",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);

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
            // Restart AI audio + restore wake word
            ResumeAudioService();
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

        // Restore AI audio service + wake word
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
            ResumeAudioService();
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
        ResumeAudioService();
        ShowNotify("Play Done");
    }

    void ShowNotify(const char *msg) {
        ShowNotify(msg, 3000);
    }
    void ShowNotify(const char *msg, int duration_ms) {
        auto display = Board::GetInstance().GetDisplay();
        if (display) display->ShowNotification(msg, duration_ms);
    }

    // Restart the AI audio service AND restore the input mode that was active
    // before we paused it (music/recording/playback all call audio.Stop() then
    // audio.Start()). AudioService::Start() clears the wake-word event bit
    // (audio_service.cc:78) and does NOT re-enable it; wake-word detection is
    // normally re-armed only on a device-state transition to Idle
    // (application.cc:816). Since pausing audio does not change the device
    // state, we must re-arm it here or the device goes deaf after playback.
    void ResumeAudioService() {
        auto& app = Application::GetInstance();
        auto& audio = app.GetAudioService();
        audio.Start();
        // Restore the input mode matching the current device state (mirrors
        // Application::HandleStateChangedEvent).
        DeviceState state = app.GetDeviceState();
        if (state == kDeviceStateIdle) {
            audio.EnableVoiceProcessing(false);
            audio.EnableWakeWordDetection(true);
            ESP_LOGI(TAG, "ResumeAudioService: Idle -> wake word re-armed");
        } else if (state == kDeviceStateListening) {
            audio.EnableVoiceProcessing(true);
            audio.EnableWakeWordDetection(false);
            ESP_LOGI(TAG, "ResumeAudioService: Listening -> voice processing re-armed");
        }
        // Other states (Connecting/Speaking/Upgrading...) are transient and
        // will be re-armed by the next state transition.
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

    // ================= ChatLog management (list/summary/play/delete) =================
    // Mirrors the recording management pattern above. ChatLog files live in
    // /sdcard/logs/chatlogs/ as chat_<stamp>_<topic>.txt (JSONL transcript) +
    // chat_<stamp>_<topic>.wav (24kHz/2ch/16bit PCM; ch0=mic, ch1=AI speaker).
    // The WAV format is identical to recordings, so playback reuses the same
    // codec->OutputData path; only the downmix differs (chatlog supports
    // mixed/mic/ai channel selection).

    // Build a full path under /sdcard/logs/chatlogs/<base>.<ext>. The caller
    // may pass a filename WITH or WITHOUT extension; the .txt/.wav is replaced
    // by <ext>. Keeps path operations DRY across list/summary/play/delete.
    static void BuildChatlogPath(char *buf, size_t buf_size, const char *filename, const char *ext) {
        // Strip any existing extension from filename to get the base name.
        char base[160];
        strncpy(base, filename, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        snprintf(buf, buf_size, "/sdcard/logs/chatlogs/%.*s.%s",
                 (int)(sizeof(base) - 1), base, ext);
    }

    // Derive a human-readable topic from a chatlog filename by extracting the
    // suffix after the second underscore (chat_<stamp>_<topic>.txt -> <topic>).
    // Returns "chat" if no topic suffix is present.
    static std::string TopicFromChatlogName(const char *filename) {
        const char *p = filename;
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

    // List system log files (ESP_LOG tee, log_YYYYMMDD.txt) under /sdcard/logs/
    // to serial. These are the daily-rotating system logs, distinct from chatlogs.
    void ListSystemLogs() {
        if (!InitializeSdCard()) {
            printf("SYSLOGS: no SD card\n");
            return;
        }
        DIR *dir = opendir("/sdcard/logs");
        if (!dir) {
            printf("SYSLOGS: no logs dir\n");
            return;
        }
        int count = 0;
        struct dirent *ent;
        printf("SYSLOGS_START\n");
        while ((ent = readdir(dir)) != NULL) {
            if (strncmp(ent->d_name, "log_", 4) == 0 && strstr(ent->d_name, ".txt")) {
                char path[300];
                snprintf(path, sizeof(path), "/sdcard/logs/%.*s",
                         (int)(sizeof(path) - 14), ent->d_name);
                struct stat st;
                long size = 0;
                if (stat(path, &st) == 0) size = st.st_size;
                printf("%s %ldB\n", ent->d_name, size);
                count++;
            }
        }
        closedir(dir);
        printf("SYSLOGS_END (%d files)\n", count);
        fflush(stdout);
    }

    // Build JSON list of system log files (log_YYYYMMDD.txt). Returns name + size.
    cJSON *ListSystemLogsJson() {
        cJSON *json = cJSON_CreateObject();
        cJSON *files = cJSON_CreateArray();
        if (!InitializeSdCard()) {
            cJSON_AddStringToObject(json, "error", "no SD card");
            cJSON_AddItemToObject(json, "system_logs", files);
            return json;
        }
        DIR *dir = opendir("/sdcard/logs");
        if (!dir) {
            cJSON_AddStringToObject(json, "error", "no logs directory");
            cJSON_AddItemToObject(json, "system_logs", files);
            return json;
        }
        struct {
            char name[64];
            long size;
        } list[40];
        int count = 0;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && count < 40) {
            if (strncmp(ent->d_name, "log_", 4) == 0 && strstr(ent->d_name, ".txt")) {
                char path[300];
                snprintf(path, sizeof(path), "/sdcard/logs/%.*s",
                         (int)(sizeof(path) - 14), ent->d_name);
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
        // Newest first (log_YYYYMMDD sorts chronologically) -> reverse
        int max_show = count < 20 ? count : 20;
        for (int i = count - 1, j = 0; i >= 0 && j < max_show; i--, j++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", list[i].name);
            cJSON_AddNumberToObject(item, "size", list[i].size);
            cJSON_AddItemToArray(files, item);
        }
        cJSON_AddStringToObject(json, "directory", "/sdcard/logs");
        cJSON_AddNumberToObject(json, "count", count);
        cJSON_AddItemToObject(json, "system_logs", files);
        return json;
    }

    // List chatlog .txt files to serial (mirrors ListRecordings). Reports the
    // paired .wav size/duration when present.
    void ListChatlogs() {
        if (!InitializeSdCard()) {
            printf("CHATLOGS: no SD card\n");
            return;
        }
        DIR *dir = opendir("/sdcard/logs/chatlogs");
        if (!dir) {
            printf("CHATLOGS: no chatlogs dir\n");
            return;
        }
        int count = 0;
        struct dirent *ent;
        printf("CHATLOGS_START\n");
        while ((ent = readdir(dir)) != NULL) {
            if (strncmp(ent->d_name, "chat_", 5) == 0 && strstr(ent->d_name, ".txt")) {
                char txt_path[256];
                BuildChatlogPath(txt_path, sizeof(txt_path), ent->d_name, "txt");
                struct stat st;
                long txt_size = 0;
                if (stat(txt_path, &st) == 0) txt_size = st.st_size;
                // Paired .wav size + duration
                char wav_path[256];
                BuildChatlogPath(wav_path, sizeof(wav_path), ent->d_name, "wav");
                long wav_size = 0;
                float dur = 0.0f;
                if (stat(wav_path, &st) == 0) {
                    wav_size = st.st_size;
                    dur = (wav_size > 44) ? (float)(wav_size - 44) / (24000 * 2 * 2) : 0.0f;
                }
                printf("%s txt=%ldB wav=%ldB %.1fs\n", ent->d_name, txt_size, wav_size, dur);
                count++;
            }
        }
        closedir(dir);
        printf("CHATLOGS_END (%d files)\n", count);
        fflush(stdout);
    }

    // Build JSON list of chatlogs (newest first, max 10). Each entry reports the
    // .txt name, txt_size, paired wav_size, duration_seconds, and started_at
    // (parsed from the first JSONL line, which is the real start time -- the
    // filename timestamp is the session END time due to RenameFilesWithTopic).
    cJSON *ListChatlogsJson() {
        cJSON *json = cJSON_CreateObject();
        cJSON *files = cJSON_CreateArray();
        if (!InitializeSdCard()) {
            cJSON_AddStringToObject(json, "error", "no SD card");
            cJSON_AddItemToObject(json, "chatlogs", files);
            return json;
        }
        DIR *dir = opendir("/sdcard/logs/chatlogs");
        if (!dir) {
            cJSON_AddStringToObject(json, "error", "no chatlogs directory");
            cJSON_AddItemToObject(json, "chatlogs", files);
            return json;
        }
        struct {
            char name[160];
            long txt_size;
            long wav_size;
            float duration;
            char started_at[24];
        } list[20];
        int count = 0;
        struct dirent *ent;
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
                // Parse first JSONL line's "ts" for the real start time.
                char started_at[24] = "";
                FILE *tf = fopen(txt_path, "r");
                if (tf) {
                    char first_line[512];
                    if (fgets(first_line, sizeof(first_line), tf)) {
                        cJSON *line = cJSON_Parse(first_line);
                        if (line) {
                            cJSON *ts = cJSON_GetObjectItem(line, "ts");
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
        // Newest first (names sort chronologically by end-time stamp) -> reverse
        int max_show = count < 10 ? count : 10;
        for (int i = count - 1, j = 0; i >= 0 && j < max_show; i--, j++) {
            cJSON *item = cJSON_CreateObject();
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

    // Parse a chatlog .txt (JSONL) and return a JSON summary. Each line is
    // {"ts","role","text"}. Caps at 50 turns to bound the response size. Lines
    // that fail to parse (e.g. half-written on power loss) are skipped.
    cJSON *GetChatlogSummaryJson(const char *filename) {
        cJSON *json = cJSON_CreateObject();
        cJSON *turns = cJSON_CreateArray();
        if (!InitializeSdCard()) {
            cJSON_AddStringToObject(json, "error", "no SD card");
            cJSON_AddItemToObject(json, "turns", turns);
            return json;
        }
        char txt_path[256];
        BuildChatlogPath(txt_path, sizeof(txt_path), filename, "txt");
        FILE *f = fopen(txt_path, "r");
        if (!f) {
            cJSON_AddStringToObject(json, "error", "file not found");
            cJSON_AddItemToObject(json, "turns", turns);
            return json;
        }
        cJSON_AddStringToObject(json, "filename", filename);
        cJSON_AddStringToObject(json, "topic", TopicFromChatlogName(filename).c_str());
        char line[1024];
        int parsed = 0;
        const int kMaxTurns = 50;
        while (fgets(line, sizeof(line), f) != NULL && parsed < kMaxTurns) {
            cJSON *obj = cJSON_Parse(line);
            if (obj) {
                cJSON *role = cJSON_GetObjectItem(obj, "role");
                cJSON *text = cJSON_GetObjectItem(obj, "text");
                cJSON *ts = cJSON_GetObjectItem(obj, "ts");
                if (cJSON_IsString(role) && cJSON_IsString(text)) {
                    cJSON *turn = cJSON_CreateObject();
                    if (cJSON_IsString(ts)) {
                        cJSON_AddStringToObject(turn, "ts", ts->valuestring);
                    }
                    cJSON_AddStringToObject(turn, "role", role->valuestring);
                    cJSON_AddStringToObject(turn, "text", text->valuestring);
                    cJSON_AddItemToArray(turns, turn);
                    parsed++;
                }
                cJSON_Delete(obj);
            }
        }
        fclose(f);
        cJSON_AddNumberToObject(json, "turn_count", parsed);
        if (parsed >= kMaxTurns) {
            cJSON_AddBoolToObject(json, "truncated", true);
        }
        cJSON_AddItemToObject(json, "turns", turns);
        return json;
    }

    // Play a chatlog WAV with a selectable channel mode. The chatlog WAV is
    // 24kHz/2ch/16bit (identical to recordings). channel_mode:
    //   0 = mixed: average both channels (mic + AI speaker) -> hear both sides
    //   1 = mic:   channel 0 only (user's microphone)
    //   2 = ai:    channel 1 only (AI speaker / AEC reference)
    // Reuses the recording playback path (pause AI audio -> codec output ->
    // stream -> resume). The play loop checks chatlog_stop_ so a future stop
    // tool (or replay) can interrupt playback.
    void PlayChatlogPath(const char *path, int channel_mode) {
        auto& app = Application::GetInstance();
        auto& audio = app.GetAudioService();
        auto codec = Board::GetInstance().GetAudioCodec();

        audio.Stop();
        vTaskDelay(pdMS_TO_TICKS(100));
        codec->EnableOutput(true);
        codec->EnableInput(false);

        FILE *f = fopen(path, "rb");
        if (!f) {
            ESP_LOGE(TAG, "Chatlog playback: cannot open %s errno=%d", path, errno);
            ShowNotify("Play Failed");
            ResumeAudioService();
            chatlog_playing_ = false;
            return;
        }
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 44, SEEK_SET);  // skip WAV header
        long data_size = (file_size > 44) ? (file_size - 44) : 0;
        const char *mode_name = (channel_mode == 1) ? "mic" : (channel_mode == 2) ? "ai" : "mixed";
        ESP_LOGI(TAG, "Chatlog playback start: %s (%s, %ld bytes, %.1fs)",
                 path, mode_name, file_size, (float)data_size / (24000 * 2 * 2));

        std::vector<int16_t> stereo(1024 * 2);
        std::vector<int16_t> mono(1024);
        size_t n;
        long played = 0;
        int last_pct = -1;
        ShowNotify("Playing... 0%");
        while (chatlog_playing_ && (n = fread(stereo.data(), 1, stereo.size() * sizeof(int16_t), f)) > 0) {
            size_t frames = n / (2 * sizeof(int16_t));
            for (size_t i = 0; i < frames; i++) {
                int16_t ch0 = stereo[i * 2];      // mic
                int16_t ch1 = stereo[i * 2 + 1];  // AI speaker (AEC ref)
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
                ESP_LOGI(TAG, "Chatlog playback progress: %d%%", last_pct);
            }
        }
        fclose(f);
        ESP_LOGI(TAG, "Chatlog playback done: %s (%ld bytes, stopped=%d)",
                 path, played, (int)!chatlog_playing_);

        codec->EnableOutput(false);
        ResumeAudioService();
        chatlog_playing_ = false;
        ShowNotify("Play Done");
    }

    // Play a chatlog by filename. Resolves the paired .wav, guards against
    // concurrent playback (music/chatlog), and spawns the play task.
    // channel_mode: 0=mixed, 1=mic, 2=ai.
    bool PlayChatlogByName(const char *filename, int channel_mode) {
        if (!InitializeSdCard()) return false;
        if (music_playing_ || chatlog_playing_) return false;
        char wav_path[256];
        BuildChatlogPath(wav_path, sizeof(wav_path), filename, "wav");
        struct stat st;
        if (stat(wav_path, &st) != 0) return false;  // paired .wav must exist
        static char play_path[256];
        strncpy(play_path, wav_path, sizeof(play_path) - 1);
        play_path[sizeof(play_path) - 1] = '\0';
        chatlog_playing_ = true;
        chatlog_stop_ = false;
        xTaskCreatePinnedToCore([](void *arg) {
            auto board = (CustomBoard *)arg;
            board->PlayChatlogPath(play_path, board->chatlog_channel_mode_);
            vTaskDelete(NULL);
        }, "chatlog_play", 8 * 1024, this, 3, NULL, 1);
        return true;
    }

    // Delete a chatlog by .txt filename; also removes the paired .wav.
    bool DeleteChatlogByName(const char *filename) {
        if (!InitializeSdCard()) return false;
        char txt_path[256], wav_path[256];
        BuildChatlogPath(txt_path, sizeof(txt_path), filename, "txt");
        BuildChatlogPath(wav_path, sizeof(wav_path), filename, "wav");
        struct stat st;
        if (stat(txt_path, &st) != 0) return false;  // .txt must exist
        bool ok = (unlink(txt_path) == 0);
        unlink(wav_path);  // best-effort .wav cleanup (ok if absent)
        if (ok) {
            ESP_LOGI(TAG, "Deleted chatlog: %s (+wav)", txt_path);
            return true;
        }
        ESP_LOGE(TAG, "Chatlog delete failed %s errno=%d", txt_path, errno);
        return false;
    }

    // ================= Music playback (MP3/MP4/AAC from /sdcard/music) =================
    // Decodes via esp_audio_codec simple decoder, downmixes stereo->mono,
    // resamples to the codec rate (24kHz) with LinearResample, then feeds PCM to
    // codec->OutputData(). Mirrors the recording play/stop/list patterns so the
    // MCP tools behave identically (self.list_music / self.play_music / ...).

    // True if a filename is a supported music file (.mp3 / .aac / .m4a).
    static bool IsMusicFile(const char* name) {
        return strstr(name, ".mp3") || strstr(name, ".MP3") ||
               strstr(name, ".aac") || strstr(name, ".AAC") ||
               strstr(name, ".m4a") || strstr(name, ".M4A");
    }

    void ListMusic() {
        if (!InitializeSdCard()) {
            printf("MUSIC: no SD card\n");
            return;
        }
        DIR *dir = opendir("/sdcard/music");
        if (!dir) {
            printf("MUSIC: no music directory\n");
            return;
        }
        int count = 0;
        struct dirent *ent;
        printf("MUSIC_START\n");
        while ((ent = readdir(dir)) != NULL) {
            if (IsMusicFile(ent->d_name)) {
                char path[300];
                snprintf(path, sizeof(path), "/sdcard/music/%.240s", ent->d_name);
                struct stat st;
                long size = 0;
                if (stat(path, &st) == 0) size = st.st_size;
                printf("%s %ldB\n", ent->d_name, size);
                count++;
            }
        }
        closedir(dir);
        printf("MUSIC_END (%d files)\n", count);
        fflush(stdout);
    }

    // Build JSON list of music files (alphabetical) for MCP tool
    cJSON *ListMusicJson() {
        cJSON *json = cJSON_CreateObject();
        cJSON *files = cJSON_CreateArray();
        if (!InitializeSdCard()) {
            cJSON_AddStringToObject(json, "error", "no SD card");
            cJSON_AddItemToObject(json, "music", files);
            return json;
        }
        DIR *dir = opendir("/sdcard/music");
        if (!dir) {
            cJSON_AddStringToObject(json, "error", "no music directory");
            cJSON_AddItemToObject(json, "music", files);
            return json;
        }
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (IsMusicFile(ent->d_name)) {
                char path[300];
                snprintf(path, sizeof(path), "/sdcard/music/%.240s", ent->d_name);
                struct stat st;
                long size = 0;
                if (stat(path, &st) == 0) size = st.st_size;
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "name", ent->d_name);
                cJSON_AddNumberToObject(item, "size", size);
                cJSON_AddItemToArray(files, item);
            }
        }
        closedir(dir);
        cJSON_AddNumberToObject(json, "count", cJSON_GetArraySize(files));
        cJSON_AddItemToObject(json, "music", files);
        return json;
    }

    // Stop any currently playing music (sets flag; playback loop checks it)
    void StopMusic() {
        music_stop_ = true;
    }

    // Linear-interpolation resampler (supports arbitrary rate pairs incl. 44.1k).
    // OpusResampler/SILK only handles 8/12/16/24/48kHz, so MP3s at other rates
    // (most commonly 44.1kHz) use this simple quality-adequate converter.
    static std::vector<int16_t> LinearResample(const std::vector<int16_t>& in,
                                               int src_rate, int dst_rate) {
        if (src_rate <= 0 || dst_rate <= 0 || src_rate == dst_rate) {
            return in;
        }
        // Output length = in_len * dst / src (round). Use uint64 to avoid overflow.
        size_t in_len = in.size();
        size_t out_len = (size_t)(((uint64_t)in_len * dst_rate) / src_rate);
        if (out_len == 0) out_len = 1;
        std::vector<int16_t> out(out_len);
        if (in_len == 1) {
            std::fill(out.begin(), out.end(), in[0]);
            return out;
        }
        double step = (double)src_rate / (double)dst_rate;  // input samples per output sample
        for (size_t i = 0; i < out_len; i++) {
            double pos = (double)i * step;                  // fractional position in input
            size_t i0 = (size_t)pos;
            size_t i1 = i0 + 1;
            if (i0 >= in_len) { out[i] = in[in_len - 1]; continue; }
            if (i1 >= in_len) { out[i] = in[i0]; continue; }
            double frac = pos - (double)i0;
            out[i] = (int16_t)(in[i0] * (1.0 - frac) + in[i1] * frac);
        }
        return out;
    }

    // Play an MP3 file: decode + downmix + resample -> codec output
    bool PlayMusicPath(const char *path) {
        auto& app = Application::GetInstance();
        auto& audio = app.GetAudioService();
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr) {
            ShowNotify("Music: no codec");
            return false;
        }

        // Pause AI audio service to free the I2S bus and codec (same as recording playback)
        audio.Stop();
        vTaskDelay(pdMS_TO_TICKS(100));
        codec->EnableOutput(true);
        codec->EnableInput(false);

        FILE *f = fopen(path, "rb");
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
        ESP_LOGI(TAG, "Music play start: %s (%ld bytes)", path, file_size);

        // Register decoders once (idempotent)
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();

        // Choose decoder by file extension: .mp3 -> MP3, .m4a -> M4A, .aac -> M4A
        // (AAC bare stream has no simple parser; many .aac files are actually
        // M4A/MP4 containers which the M4A parser handles, including AAC audio).
        esp_audio_simple_dec_cfg_t dec_cfg = {};
        dec_cfg.dec_cfg   = nullptr;
        dec_cfg.cfg_size  = 0;
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
        ESP_LOGI(TAG, "Music: using %s decoder", codec_name);

        // Buffers: input read chunk + PCM output (max MP3 frame ~ 4608 bytes)
        const int kReadSize = 4096;
        const int kOutBytes = 16384;  // 8192 int16 samples, enough for one frame
        uint8_t *in_buf = (uint8_t *)heap_caps_malloc(kReadSize, MALLOC_CAP_SPIRAM);
        uint8_t *out_buf = (uint8_t *)heap_caps_malloc(kOutBytes, MALLOC_CAP_SPIRAM);
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

        // Resampler: MP3 native rate (e.g. 44.1k) -> codec output rate (24k)
        // Note: OpusResampler (SILK) only supports 8/12/16/24/48kHz, so for
        // 44.1kHz MP3s we use a linear-interpolation resampler instead.
        int mp3_rate = 44100, mp3_ch = 2;
        bool need_resample = false;
        bool info_ready = false;
        uint32_t decoded_total = 0;
        uint32_t last_notify_ms = 0;
        music_stop_ = false;
        music_playing_ = true;

        // Show song name in the bottom notification once playback begins.
        // "♫" may not render on the 1-bit LCD font; use ASCII ">>" fallback.
        const char *slash = strrchr(path, '/');
        const char *song = slash ? slash + 1 : path;
        char song_info[96];
        snprintf(song_info, sizeof(song_info), ">> %s", song);
        ESP_LOGI(TAG, "Music notify: %s", song_info);
        ShowNotify(song_info, 8000);

        while (!music_stop_) {
            int rd = fread(in_buf, 1, kReadSize, f);
            if (rd <= 0) break;  // EOF

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
                    ESP_LOGI(TAG, "Music: decode buffer too small (needed %u)", out.needed_size);
                    break;  // out buf too small; next chunk handles it
                }
                if (ret != ESP_AUDIO_ERR_OK) {
                    ESP_LOGW(TAG, "Music: decode error %d (consumed %u)", ret, raw.consumed);
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
                        ESP_LOGI(TAG, "Music: MP3 %dHz %dch -> out %dHz %s", mp3_rate, mp3_ch,
                                 codec->output_sample_rate(), need_resample ? "(resampling)" : "");
                        // Show stream info in notification: rate + channel
                        char info_line[96];
                        snprintf(info_line, sizeof(info_line), "%d.%dkHz %s", mp3_rate / 1000,
                                 (mp3_rate % 1000) / 100, mp3_ch >= 2 ? "Stereo" : "Mono");
                        ESP_LOGI(TAG, "Music notify: %s", info_line);
                        ShowNotify(info_line, 4000);
                    }
                    decoded_total += out.decoded_size;
                    int samples = out.decoded_size / sizeof(int16_t);
                    int16_t *pcm = (int16_t *)out_buf;

                    // Downmix stereo -> mono (average L/R), then resample if needed
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
                        // Periodic playback progress in the notification: elapsed/total
                        uint32_t now_ms = esp_timer_get_time() / 1000;
                        if (now_ms - last_notify_ms >= 5000) {
                            last_notify_ms = now_ms;
                            // Elapsed seconds from decoded PCM (mono frames at mp3_rate)
                            uint32_t decoded_frames = decoded_total / sizeof(int16_t) / (mp3_ch >= 2 ? 2 : 1);
                            uint32_t elapsed_s = (mp3_rate > 0) ? decoded_frames / mp3_rate : 0;
                            // File byte progress vs total file size
                            long cur = ftell(f);
                            int pct = (file_size > 0) ? (int)((cur * 100) / file_size) : 0;
                            if (pct > 100) pct = 100;
                            char prog[96];
                            snprintf(prog, sizeof(prog), "%s  %d%%  %02lu:%02lu",
                                     song, pct, (unsigned long)(elapsed_s / 60),
                                     (unsigned long)(elapsed_s % 60));
                            ESP_LOGI(TAG, "Music notify: %s", prog);
                            ShowNotify(prog, 5000);
                        }
                    }
                }

                if (raw.consumed == 0) break;  // no progress, avoid infinite loop
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

    // Play an MP3 by filename from /sdcard/music (spawns task, mirrors play_recording)
    bool PlayMusicByName(const char *filename) {
        if (music_playing_) {
            return false;  // already playing
        }
        if (!InitializeSdCard()) return false;
        char path[300];
        snprintf(path, sizeof(path), "/sdcard/music/%.240s", filename);
        struct stat st;
        if (stat(path, &st) != 0) return false;
        // Spawn task to play (don't block MCP callback)
        static char music_path[300];
        snprintf(music_path, sizeof(music_path), "%s", path);
        xTaskCreatePinnedToCore([](void *arg) {
            auto board = (CustomBoard *)arg;
            board->PlayMusicPath(music_path);
            vTaskDelete(NULL);
        }, "mcp_music", 12 * 1024, this, 3, NULL, 1);
        return true;
    }

    // Delete a music file by name. Returns true on success.
    bool DeleteMusicByName(const char *filename) {
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

        // List music files on SD card
        mcp_server.AddTool("self.list_music",
            "List the music files (MP3/AAC/M4A) stored in the /sdcard/music directory on the SD card.\n"
            "Returns the filenames and sizes.\n"
            "Use this tool when the user asks what music is available, or wants to choose a song to play.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                return static_cast<cJSON*>(ListMusicJson());
            });

        // Play a music file
        mcp_server.AddTool("self.play_music",
            "Play a music file (MP3/AAC/M4A) from the /sdcard/music directory on the SD card.\n"
            "The 'filename' parameter must be a filename from self.list_music results (e.g. song.mp3).\n"
            "Use this tool when the user asks to play a song, music, or audio file.",
            PropertyList({
                Property("filename", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string filename = properties["filename"].value<std::string>();
                if (music_playing_) {
                    return std::string("Error: music is already playing. Stop it first with self.stop_music.");
                }
                if (PlayMusicByName(filename.c_str())) {
                    return std::string("Playing music: " + filename);
                }
                return std::string("Error: music file not found: " + filename);
            });

        // Stop music playback
        mcp_server.AddTool("self.stop_music",
            "Stop the currently playing music.\n"
            "Use this tool when the user asks to stop, pause, or turn off the music.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (!music_playing_) {
                    return std::string("No music is playing");
                }
                StopMusic();
                return std::string("Stopping music");
            });

        // Delete a music file
        mcp_server.AddTool("self.delete_music",
            "Delete a specific music file (MP3/AAC/M4A) from the /sdcard/music directory on the SD card.\n"
            "The 'filename' parameter must be a filename from self.list_music results (e.g. song.mp3).\n"
            "Use this tool when the user asks to delete or remove a music file.",
            PropertyList({
                Property("filename", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string filename = properties["filename"].value<std::string>();
                if (DeleteMusicByName(filename.c_str())) {
                    return std::string("Deleted music: " + filename);
                }
                return std::string("Error: music file not found or delete failed: " + filename);
            });

        // Run hardware self-test (display, buttons, SD, battery, RTC, SHTC3, audio)
        mcp_server.AddTool("self.run_self_test",
            "Run a hardware self-test on the device: verifies the display, buttons, SD card read/write, "
            "battery ADC, RTC clock, SHTC3 temperature/humidity sensor and audio echo loopback.\n"
            "Returns a summary of pass/fail results per item. Use this when the user asks to "
            "test, check, diagnose, or self-check the device hardware.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (self_test_running_) {
                    return std::string("Self-test already running");
                }
                RunSelfTest();
                return std::string("Self-test started, results will be reported shortly");
            });

        // Fetch the last self-test result (JSON)
        mcp_server.AddTool("self.get_self_test_result",
            "Get the JSON result of the last hardware self-test run, including per-item "
            "pass/fail status and details for display, buttons, SD card, battery, RTC, "
            "temperature/humidity sensor and audio.\n"
            "Returns an empty string if no self-test has been run yet.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                std::lock_guard<std::mutex> lock(self_test_mutex_);
                if (self_test_result_.empty()) {
                    return std::string("No self-test result yet. Run self.run_self_test first.");
                }
                return self_test_result_;
            });

        // Firmware version / build / feature / flash info (voice queryable)
        mcp_server.AddTool("self.get_version_info",
            "Get the firmware version, build info (compile time, git commit, ESP-IDF version), "
            "flash size, partition layout, and the list of features supported by this device.\n"
            "Use this tool when the user asks about the firmware version, what this device can do, "
            "its features or capabilities, hardware/flash info, or build/compile info "
            "(e.g. \u201c你的版本是多少\u201d, \u201c你都有什么功能\u201d, \u201c固件信息\u201d).",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                return VersionInfo::BuildVersionInfoJson();
            });

        // ================= ChatLog management (voice queryable) =================
        // List recent chat conversation logs or system logs saved on the SD card.
        mcp_server.AddTool("self.list_chatlogs",
            "List log files saved on the SD card. The optional 'directory' parameter selects which set:\n"
            "  - \"chatlogs\" (default): recent AI conversation logs in /sdcard/logs/chatlogs/, "
            "each with topic, text/wav sizes, audio duration, and start time.\n"
            "  - \"system_logs\" or \"logs\": daily system logs in /sdcard/logs/ (log_YYYYMMDD.txt), "
            "each with filename and size. These are ESP_LOG output tee'd to SD.\n"
            "Returns up to 10 (chatlogs) or 20 (system_logs) entries, newest first.\n"
            "Use this tool when the user asks about recent conversations, chat history, "
            "system logs, or log files (e.g. \u201c最近聊了什么\u201d, \u201c对话记录\u201d, "
            "\u201c系统日志\u201d, \u201clogs目录的文件\u201d, \u201c日志文件\u201d).",
            PropertyList({
                Property("directory", kPropertyTypeString, std::string("chatlogs"))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string directory = properties["directory"].value<std::string>();
                if (directory == "system_logs" || directory == "logs") {
                    return static_cast<cJSON*>(ListSystemLogsJson());
                }
                return static_cast<cJSON*>(ListChatlogsJson());
            });

        // Get a text summary (transcript) of a specific chatlog.
        mcp_server.AddTool("self.get_chatlog_summary",
            "Get the text transcript of a specific chat conversation log.\n"
            "Returns the conversation turns (timestamp, role: user/assistant, text), up to 50 turns.\n"
            "The 'filename' parameter must be a filename from self.list_chatlogs results "
            "(e.g. chat_20260802_143000_天气查询.txt).\n"
            "Use this tool when the user asks what was said in a specific conversation "
            "(e.g. \u201c那次对话说了什么\u201d, \u201c对话内容\u201d, \u201c聊天摘要\u201d).",
            PropertyList({
                Property("filename", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string filename = properties["filename"].value<std::string>();
                return static_cast<cJSON*>(GetChatlogSummaryJson(filename.c_str()));
            });

        // Play the audio of a specific chatlog, with selectable channel mode.
        mcp_server.AddTool("self.play_chatlog_audio",
            "Play the audio recording of a specific chat conversation.\n"
            "The 'filename' parameter must be a filename from self.list_chatlogs results.\n"
            "The optional 'channel' parameter selects which side of the conversation to hear:\n"
            "  - \"mixed\" (default): both sides averaged together\n"
            "  - \"mic\": only the user's microphone\n"
            "  - \"ai\": only the AI assistant's voice\n"
            "Use this tool when the user asks to listen to a conversation recording "
            "(e.g. \u201c听一下那段对话\u201d, \u201c播放录音\u201d, \u201c只听小智的声音\u201d).",
            PropertyList({
                Property("filename", kPropertyTypeString),
                Property("channel", kPropertyTypeString, std::string("mixed"))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string filename = properties["filename"].value<std::string>();
                std::string channel = properties["channel"].value<std::string>();
                int mode = 0;  // mixed
                if (channel == "mic") mode = 1;
                else if (channel == "ai") mode = 2;
                chatlog_channel_mode_ = mode;
                if (PlayChatlogByName(filename.c_str(), mode)) {
                    const char *mode_name = (mode == 1) ? "mic" : (mode == 2) ? "ai" : "mixed";
                    return std::string("Playing chatlog: " + filename + " (" + mode_name + ")");
                }
                return std::string("Error: cannot play (file not found, or music/other playback in progress): " + filename);
            });

        // Delete a chatlog (both .txt transcript and paired .wav audio).
        mcp_server.AddTool("self.delete_chatlog",
            "Delete a specific chat conversation log, including its text transcript and audio file.\n"
            "The 'filename' parameter must be a filename from self.list_chatlogs results.\n"
            "Use this tool when the user asks to delete or remove a conversation record "
            "(e.g. \u201c删掉那条对话\u201d, \u201c清除记录\u201d).",
            PropertyList({
                Property("filename", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string filename = properties["filename"].value<std::string>();
                if (DeleteChatlogByName(filename.c_str())) {
                    return std::string("Deleted chatlog: " + filename);
                }
                return std::string("Error: chatlog file not found or delete failed: " + filename);
            });

        // ================= WiFi HTTP file server (wireless file download) =================
        mcp_server.AddTool("self.start_file_server",
            "Start a WiFi HTTP file server so the user can download SD card files (logs, "
            "chatlogs, recordings, music) wirelessly via a web browser, avoiding serial port "
            "conflicts.\n"
            "Returns the URL (e.g. http://192.168.1.100/) the user should open in a browser.\n"
            "Use this tool when the user asks to download or transfer files from the device "
            "(e.g. \u201c下载日志\u201d, \u201c获取文件\u201d, \u201c导出录音\u201d).",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto &srv = HttpFileServer::GetInstance();
                if (srv.IsRunning()) {
                    return std::string("File server already running: " + srv.GetUrl());
                }
                if (srv.Start(80)) {
                    return std::string("File server started: " + srv.GetUrl());
                }
                return std::string("Error: failed to start file server (WiFi connected?)");
            });

        mcp_server.AddTool("self.stop_file_server",
            "Stop the WiFi HTTP file server.\n"
            "Use this tool when the user asks to stop or close the file download service.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto &srv = HttpFileServer::GetInstance();
                if (srv.IsRunning()) {
                    srv.Stop();
                    return std::string("File server stopped");
                }
                return std::string("File server was not running");
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
        // Don't abort boot if ADC fails - battery just reports unavailable.
        if (adc_oneshot_new_unit(&init_config, &adc1_handle) != ESP_OK) {
            ESP_LOGW(TAG, "ADC init failed, battery level unavailable");
            adc1_handle = nullptr;
            return;
        }
        adc_oneshot_chan_cfg_t chan_config = {};
        chan_config.atten = ADC_ATTEN_DB_12;
        chan_config.bitwidth = ADC_BITWIDTH_12;
        ESP_ERROR_CHECK_WITHOUT_ABORT(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &chan_config));
        adc_cali_curve_fitting_config_t cali_config = {};
        cali_config.unit_id = ADC_UNIT_1;
        cali_config.atten = ADC_ATTEN_DB_12;
        cali_config.bitwidth = ADC_BITWIDTH_12;
        if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) != ESP_OK) {
            ESP_LOGW(TAG, "ADC calibration init failed, battery level unavailable");
            cali_handle = nullptr;
        }
    }

    // SHTC3 temperature/humidity sensor (I2C addr 0x70)
    bool ReadShtc3(float& temp_c, float& humidity_pct) {
        if (!GetI2cDevice(0x70, &shtc3_dev_)) return false;
        uint8_t wake_cmd[] = {0x35, 0x17};
        i2c_master_transmit(shtc3_dev_, wake_cmd, 2, -1);
        vTaskDelay(pdMS_TO_TICKS(1));
        uint8_t meas_cmd[] = {0x7C, 0xA2};
        i2c_master_transmit(shtc3_dev_, meas_cmd, 2, -1);
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t raw[6];
        esp_err_t ret = i2c_master_receive(shtc3_dev_, raw, 6, -1);
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
                } else if (strcmp(line, "SELFTEST") == 0) {
                    ESP_LOGI(TAG, "Self-test requested via serial");
                    board->RunSelfTest();
                } else if (strcmp(line, "CHATLOG") == 0) {
                    ESP_LOGI(TAG, "Chat log debug requested via serial");
                    Application::GetInstance().DebugChatLog();
                } else if (strcmp(line, "CHATLOGLIST") == 0) {
                    ESP_LOGI(TAG, "List chatlogs requested via serial");
                    board->ListChatlogs();
                } else if (strcmp(line, "SYSLOGLIST") == 0) {
                    ESP_LOGI(TAG, "List system logs requested via serial");
                    board->ListSystemLogs();
                } else if (strcmp(line, "HTTPSTART") == 0) {
                    ESP_LOGI(TAG, "Start HTTP file server requested via serial");
                    auto &srv = HttpFileServer::GetInstance();
                    if (srv.IsRunning()) {
                        printf("HTTP: already running: %s\n", srv.GetUrl().c_str());
                    } else if (srv.Start(80)) {
                        printf("HTTP: started: %s\n", srv.GetUrl().c_str());
                    } else {
                        printf("HTTP: start failed (WiFi connected?)\n");
                    }
                    fflush(stdout);
                } else if (strcmp(line, "HTTPSTOP") == 0) {
                    ESP_LOGI(TAG, "Stop HTTP file server requested via serial");
                    HttpFileServer::GetInstance().Stop();
                    printf("HTTP: stopped\n");
                    fflush(stdout);
                } else if (strcmp(line, "MUSICLIST") == 0) {
                    ESP_LOGI(TAG, "List music requested via serial");
                    board->ListMusic();
                } else if (strncmp(line, "MUSICPLAY ", 10) == 0) {
                    const char *name = line + 10;
                    ESP_LOGI(TAG, "Play music requested via serial: %s", name);
                    if (board->music_playing_) {
                        printf("MUSIC: already playing\n");
                    } else if (board->PlayMusicByName(name)) {
                        printf("MUSIC: playing %s\n", name);
                    } else {
                        printf("MUSIC: file not found: %s\n", name);
                    }
                    fflush(stdout);
                } else if (strcmp(line, "MUSICSTOP") == 0) {
                    ESP_LOGI(TAG, "Stop music requested via serial");
                    board->StopMusic();
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

    // ================= Self-test (自检) =================
    // Runs a battery of hardware checks and reports pass/fail on the display
    // and via an MCP tool. Triggered by the "self.run_self_test" MCP tool or
    // the serial command "SELFTEST".
    //   Test items: display LUT, buttons, SD card R/W, battery ADC, RTC,
    //               SHTC3 temp/humidity, audio echo loopback.
    struct SelfTestItem {
        const char* name;
        bool passed;
        char detail[48];
    };

    static void SelfTestTask(void *arg) {
        auto board = (CustomBoard *)arg;
        board->RunSelfTestImpl();
        vTaskDelete(NULL);
    }

    void RunSelfTest() {
        if (self_test_running_) {
            ShowNotify("Self-test already running", 2000);
            return;
        }
        self_test_running_ = true;
        xTaskCreatePinnedToCore(SelfTestTask, "self_test", 8 * 1024, this, 3, NULL, 1);
    }

    void RunSelfTestImpl() {
        const int kNumItems = 7;
        SelfTestItem items[kNumItems] = {};
        int num_items = 0;

        // 1. Display: verify pixel LUT round-trip (write pixel, read it back)
        {
            SelfTestItem &it = items[num_items++];
            it.name = "Display";
            bool ok = false;
            if (display_ != nullptr) {
                // Take the display lock so direct RLCD_* access cannot race with
                // the LVGL flush callback (both drive the same framebuffer).
                DisplayLockGuard lock(display_);
                int w = display_->GetWidth();
                int h = display_->GetHeight();
                // Save a corner pixel and set the opposite one to verify LUT mapping
                display_->RLCD_ColorClear(ColorWhite);
                display_->RLCD_SetPixel(0, 0, ColorBlack);
                display_->RLCD_SetPixel(w - 1, h - 1, ColorBlack);
                display_->RLCD_Display();
                ok = (display_->GetPixel(0, 0) == ColorBlack) &&
                     (display_->GetPixel(w - 1, h - 1) == ColorBlack);
                display_->RLCD_ColorClear(ColorWhite);
                display_->RLCD_Display();
            }
            it.passed = ok;
            snprintf(it.detail, sizeof(it.detail), "%s", ok ? "LUT ok" : "LUT fail");
        }

        // 2. Buttons: read GPIO levels of BOOT and KEY
        {
            SelfTestItem &it = items[num_items++];
            it.name = "Buttons";
            int boot = gpio_get_level(BOOT_BUTTON_GPIO);
            int key  = gpio_get_level(GPIO_NUM_18);
            it.passed = true;  // levels are informational, always "pass"
            snprintf(it.detail, sizeof(it.detail), "BOOT=%d KEY=%d", boot, key);
        }

        // 3. SD card: write + readback a probe file
        {
            SelfTestItem &it = items[num_items++];
            it.name = "SDCard";
            bool ok = false;
            if (InitializeSdCard()) {
                const char *path = "/sdcard/selftest.txt";
                const char *data = "xiaozhi-selftest";
                FILE *f = fopen(path, "wb");
                if (f != nullptr) {
                    size_t wr = fwrite(data, 1, strlen(data), f);
                    fclose(f);
                    char rbuf[32] = {0};
                    f = fopen(path, "rb");
                    if (f != nullptr) {
                        size_t rd = fread(rbuf, 1, sizeof(rbuf) - 1, f);
                        fclose(f);
                        ok = (wr == strlen(data)) && (rd == strlen(data)) && (strcmp(rbuf, data) == 0);
                    }
                    unlink(path);
                }
            }
            it.passed = ok;
            snprintf(it.detail, sizeof(it.detail), "%s", ok ? "RW ok" : "failed");
        }

        // 4. Battery ADC
        {
            SelfTestItem &it = items[num_items++];
            it.name = "Battery";
            int level; bool charging, discharging;
            bool ok = GetBatteryLevel(level, charging, discharging);
            it.passed = ok;
            if (ok) {
                snprintf(it.detail, sizeof(it.detail), "%d%% chg=%d", level, charging ? 1 : 0);
            } else {
                snprintf(it.detail, sizeof(it.detail), "%s", "ADC n/a");
            }
        }

        // 5. RTC: read time and validate year range (2000-2099)
        {
            SelfTestItem &it = items[num_items++];
            it.name = "RTC";
            struct tm tm;
            bool ok = RtcReadTime(&tm);
            it.passed = ok && tm.tm_year >= 100 && tm.tm_year < 200;
            if (ok) {
                snprintf(it.detail, sizeof(it.detail), "%04d-%02d-%02d %02d:%02d",
                         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
            } else {
                snprintf(it.detail, sizeof(it.detail), "%s", "I2C err");
            }
        }

        // 6. SHTC3 temperature/humidity
        {
            SelfTestItem &it = items[num_items++];
            it.name = "SHTC3";
            float temp, hum;
            bool ok = ReadShtc3(temp, hum);
            it.passed = ok && (hum >= 0.0f && hum <= 100.0f) && (temp > -40.0f && temp < 125.0f);
            if (ok) {
                snprintf(it.detail, sizeof(it.detail), "%.1fC %.0f%%", temp, hum);
            } else {
                snprintf(it.detail, sizeof(it.detail), "%s", "I2C err");
            }
        }

        // 7. Audio echo loopback: record short PCM, play it back
        {
            SelfTestItem &it = items[num_items++];
            it.name = "Audio";
            auto& app = Application::GetInstance();
            // Skip disruptive audio test while the device is actively chatting/recording
            if (app.GetDeviceState() != kDeviceStateIdle &&
                app.GetDeviceState() != kDeviceStateStarting &&
                app.GetDeviceState() != kDeviceStateWifiConfiguring) {
                it.passed = true;  // skipped, not a failure
                snprintf(it.detail, sizeof(it.detail), "%s", "skipped (busy)");
            } else {
                it.passed = SelfTestAudioEcho(it.detail, sizeof(it.detail));
            }
        }

        // Report results to display + serial + JSON
        cJSON *root = cJSON_CreateObject();
        int passed_count = 0;
        for (int i = 0; i < num_items; i++) {
            const SelfTestItem &it = items[i];
            if (it.passed) passed_count++;
            ESP_LOGI(TAG, "SELFTEST %-8s %s - %s", it.name, it.passed ? "PASS" : "FAIL", it.detail);
            cJSON *item = cJSON_CreateObject();
            cJSON_AddBoolToObject(item, "pass", it.passed);
            cJSON_AddStringToObject(item, "detail", it.detail);
            cJSON_AddItemToObject(root, it.name, item);
        }
        bool all_passed = (passed_count == num_items);
        cJSON_AddNumberToObject(root, "passed", passed_count);
        cJSON_AddNumberToObject(root, "total", num_items);
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) {
            {
                std::lock_guard<std::mutex> lock(self_test_mutex_);
                self_test_result_ = json_str;
            }
            cJSON_free(json_str);
        }
        cJSON_Delete(root);

        char summary[96];
        snprintf(summary, sizeof(summary), "SelfTest: %d/%d %s", passed_count, num_items, all_passed ? "ALL PASS" : "FAIL");
        ESP_LOGI(TAG, "SELFTEST %s", summary);
        ShowNotify(summary, 5000);
        self_test_running_ = false;
    }

    // Record ~0.3s of PCM (2ch 16bit) then play it back through the speaker.
    // Returns true if both capture and playback paths completed without error.
    bool SelfTestAudioEcho(char *detail, size_t detail_len) {
        auto& app = Application::GetInstance();
        auto& audio = app.GetAudioService();
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr) {
            snprintf(detail, detail_len, "%s", "no codec");
            return false;
        }

        // Pause AI audio service to free the I2S bus and codec (same as recording)
        audio.Stop();
        vTaskDelay(pdMS_TO_TICKS(100));
        codec->EnableInput(true);
        codec->EnableOutput(false);

        // Capture ~0.3s of stereo (mic + ref) @24kHz
        const int sample_rate = 24000;
        const int channels = 2;
        const int capture_ms = 300;
        const size_t capture_samples = (size_t)(sample_rate * capture_ms / 1000) * channels;
        std::vector<int16_t> pcm(capture_samples);
        size_t got = 0;
        const int chunk_samples = 512 * channels;
        std::vector<int16_t> chunk(chunk_samples);
        uint32_t start_ms = esp_timer_get_time() / 1000;
        while (got < capture_samples && (esp_timer_get_time() / 1000 - start_ms) < 2000) {
            if (codec->InputData(chunk)) {
                size_t n = chunk.size();
                if (got + n > capture_samples) n = capture_samples - got;
                memcpy(pcm.data() + got, chunk.data(), n * sizeof(int16_t));
                got += n;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        bool capture_ok = (got > capture_samples / 10);  // at least 10% of target

        // Play back mic channel (channel 0) as mono
        codec->EnableInput(false);
        codec->EnableOutput(true);
        codec->SetOutputVolume(70);
        std::vector<int16_t> mono(1024);
        size_t played = 0;
        uint32_t play_start_ms = esp_timer_get_time() / 1000;
        size_t frames = got / channels;
        while (played < frames && (esp_timer_get_time() / 1000 - play_start_ms) < 2000) {
            size_t n = frames - played;
            if (n > mono.size()) n = mono.size();
            for (size_t i = 0; i < n; i++) {
                mono[i] = pcm[(played + i) * channels];  // extract mic channel
            }
            mono.resize(n);
            codec->OutputData(mono);
            mono.resize(1024);
            played += n;
        }
        bool playback_ok = (played > frames / 2);  // at least half the frames written

        codec->EnableOutput(false);
        ResumeAudioService();

        snprintf(detail, detail_len, "cap=%dms play=%dms", (int)(got / channels / (sample_rate / 1000)),
                 (int)(played / (sample_rate / 1000)));
        return capture_ok && playback_ok;
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
#if defined(RLCD_ENABLE_KEY_LEVEL_MONITOR) && RLCD_ENABLE_KEY_LEVEL_MONITOR
        xTaskCreatePinnedToCore(KeyLevelMonitorTask, "key_mon", 2 * 1024, this, 1, NULL, 1);
#endif
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
        if (adc1_handle == nullptr || cali_handle == nullptr) return false;
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