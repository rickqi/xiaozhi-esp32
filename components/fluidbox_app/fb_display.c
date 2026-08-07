#include "fb_display.h"

#include "config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"

#define BAND_PIXELS (LCD_H_RES * BAND_ROWS)

static const char *TAG = "fluidbox_disp";

static uint16_t *s_band_buf;   // DMA pool, allocated lazily
static esp_lcd_panel_handle_t s_panel;

esp_err_t fb_panel_set_handle(esp_lcd_panel_handle_t panel)
{
    if (panel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_panel = panel;
    if (s_band_buf == NULL) {
        s_band_buf = (uint16_t*)heap_caps_malloc(BAND_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA);
        if (s_band_buf == NULL) {
            ESP_LOGE(TAG, "band buffer alloc failed");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

uint16_t *fb_acquire_band(void)
{
    return s_band_buf;
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
    return esp_lcd_panel_draw_bitmap(s_panel, 0, y0, LCD_H_RES, y1, buffer);
}

esp_err_t fb_set_brightness(uint8_t level)
{
    return ESP_OK;
}
