#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "board_pins.h"

/* Bring up I2C, the IO expander, backlight PWM and battery ADC. */
esp_err_t board_init(void);
i2c_master_bus_handle_t board_i2c_bus(void);

/* PCF8574A expander (quasi-bidirectional: write 1 to use a pin as input). */
esp_err_t board_exio_write(int bit, bool level);
esp_err_t board_exio_read(uint8_t *port);

void board_backlight_set(uint8_t percent);

/* Battery voltage in millivolts (0 on error) and charger state. */
int  board_battery_mv(void);
int  board_battery_percent(int mv);
bool board_is_charging(void);

/* microSD (FAT) on /sdcard and internal SPIFFS on /spiffs */
esp_err_t board_sd_mount(void);
void      board_sd_unmount(void);
bool      board_sd_mounted(void);
esp_err_t board_spiffs_mount(void);

/* Pause battery ADC sampling while the ADC entropy source is in use. */
void board_adc_lock(void);
void board_adc_unlock(void);
