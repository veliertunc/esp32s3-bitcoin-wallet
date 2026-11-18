#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef void (*lcd_flush_done_cb_t)(void *user);

/* Initialise SPI bus + ST7796 panel. done_cb fires (from ISR) after each
 * lcd_draw() transfer completes. */
esp_err_t lcd_init(lcd_flush_done_cb_t done_cb, void *user);

/* Push an RGB565 (big-endian / byte-swapped) block. Inclusive coordinates. */
esp_err_t lcd_draw(int x1, int y1, int x2, int y2, const void *pixels);
