#include "fb_display.h"

#include "config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"

#define BAND_PIXELS (LCD_H_RES * BAND_ROWS)

static const char *TAG = "fluidbox_disp";

static uint16_t *s_band_buf[2];  // double-buffered: one may be transmitting via SPI while the other is being drawn
static int s_next_band = 0;
static esp_lcd_panel_handle_t s_panel;

esp_err_t fb_panel_set_handle(esp_lcd_panel_handle_t panel)
{
    if (panel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_panel = panel;
    if (s_band_buf[0] == NULL) {
        for (int i = 0; i < 2; i++) {
            s_band_buf[i] = (uint16_t*)heap_caps_malloc(BAND_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA);
            if (s_band_buf[i] == NULL) {
                ESP_LOGE(TAG, "band buffer %d alloc failed", i);
                return ESP_ERR_NO_MEM;
            }
        }
    }
    return ESP_OK;
}

uint16_t *fb_acquire_band(void)
{
    return s_band_buf[s_next_band++ & 1];
}

esp_err_t fb_flush_band(int band_index, const uint16_t *buffer)
{
    const int y0 = band_index * BAND_ROWS;
    int y1 = y0 + BAND_ROWS;
    if (y1 > LCD_V_RES) {
        y1 = LCD_V_RES;
    }
    if (y0 >= y1) {
        return ESP_OK;
    }
    esp_err_t ret = esp_lcd_panel_draw_bitmap(s_panel, 0, y0, LCD_H_RES, y1, buffer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "draw_bitmap band %d failed: %s", band_index, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t fb_set_brightness(uint8_t level)
{
    return ESP_OK;
}
