#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t touch_init(void);

/* Returns true while the panel is touched; coordinates in display pixels. */
bool touch_read(uint16_t *x, uint16_t *y);
