#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_log.h>
#include <mbedtls/base64.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "custom_lcd_display.h"
#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
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
        temp_c = -45.0f + 175.0f * t_raw / 65535.0f;
        humidity_pct = 100.0f * h_raw / 65535.0f;
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
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // One-shot auto screenshot ~12s after boot (screen settled)
    static void AutoScreenshotTask(void *arg) {
        auto board = (CustomBoard *)arg;
        vTaskDelay(pdMS_TO_TICKS(12000));
        ESP_LOGI(TAG, "Auto screenshot after boot");
        board->TakeScreenshot();
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
        xTaskCreatePinnedToCore(ScreenshotCmdTask, "scr_cmd", 6 * 1024, this, 1, NULL, 1);
        xTaskCreatePinnedToCore(AutoScreenshotTask, "scr_auto", 6 * 1024, this, 1, NULL, 1);
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

DECLARE_BOARD(CustomBoard);