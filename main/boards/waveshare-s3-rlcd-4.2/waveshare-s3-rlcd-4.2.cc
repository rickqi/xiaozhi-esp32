#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_log.h>
#include <mbedtls/base64.h>
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
};

DECLARE_BOARD(CustomBoard);