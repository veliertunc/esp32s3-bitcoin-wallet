/*
 * Hynitron CST826 capacitive touch controller (I2C 0x15).
 * Register 0x02: [num_points, XH(4bit), XL, YH(4bit), YL, ...]
 */
#include "touch_cst826.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "board.h"
#include "sdkconfig.h"

static const char *TAG = "cst826";
static i2c_master_dev_handle_t s_dev;

esp_err_t touch_init(void)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_CST826_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_i2c_bus(), &dev, &s_dev), TAG, "add dev");

    uint8_t reg = 0xAA, id = 0;
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &id, 1, 100) == ESP_OK) {
        ESP_LOGI(TAG, "chip id 0x%02X", id);
    } else {
        ESP_LOGW(TAG, "no answer from touch controller (it may be asleep until first touch)");
    }
    return ESP_OK;
}

bool touch_read(uint16_t *x, uint16_t *y)
{
    uint8_t reg = 0x02;
    uint8_t d[5];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, d, sizeof(d), 50) != ESP_OK) {
        return false;
    }
    uint8_t n = d[0] & 0x0F;
    if (n == 0 || n > 2) {
        return false;
    }
    int px = ((d[1] & 0x0F) << 8) | d[2];
    int py = ((d[3] & 0x0F) << 8) | d[4];
#if CONFIG_BOARD_TOUCH_MIRROR_X
    px = BOARD_LCD_H_RES - 1 - px;
#endif
#if CONFIG_BOARD_TOUCH_MIRROR_Y
    py = BOARD_LCD_V_RES - 1 - py;
#endif
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= BOARD_LCD_H_RES) px = BOARD_LCD_H_RES - 1;
    if (py >= BOARD_LCD_V_RES) py = BOARD_LCD_V_RES - 1;
    *x = (uint16_t)px;
    *y = (uint16_t)py;
    return true;
}
