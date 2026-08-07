#include "fb_display.h"

#include "config.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"

#define BAND_PIXELS (LCD_H_RES * BAND_ROWS)

static const char *TAG = "fluidbox_disp";

static DMA_ATTR uint16_t s_band_buf[BAND_PIXELS];
static esp_lcd_panel_handle_t s_panel;

esp_err_t fb_panel_set_handle(esp_lcd_panel_handle_t panel)
{
    if (panel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_panel = panel;
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
