#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

// Band-buffered panel writer for FluidBox that reuses the brookesia board's
// SH8601 panel handle instead of owning its own panel driver. Callers hand a
// panel handle in once (fb_panel_set_handle); the SH8601 was already brought
// up by the host firmware's LVGL port, so this layer only pushes raw RGB565
// bands through esp_lcd_panel_draw_bitmap with DMA.

esp_err_t fb_panel_set_handle(esp_lcd_panel_handle_t panel);

// Blocks until a band buffer is free, then returns it. BAND_ROWS * LCD_H_RES
// pixels. Buffers rotate per call rather than per band index, because the
// renderer skips bands that are already black on the panel, and two transfers
// in flight must never land in the same buffer.
uint16_t *fb_acquire_band(void);

// Queues an asynchronous transfer of one band. Does not block.
esp_err_t fb_flush_band(int band_index, const uint16_t *buffer);

// 0..255. The panel dims itself; there is no backlight pin on an AMOLED.
esp_err_t fb_set_brightness(uint8_t level);
