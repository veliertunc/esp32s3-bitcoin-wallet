#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

/* Initialise LCD + touch + LVGL and start the LVGL task. */
esp_err_t lvgl_port_start(void);

/* All LVGL calls from other tasks must be wrapped in lock/unlock (recursive). */
bool ui_lock(int timeout_ms);
void ui_unlock(void);
