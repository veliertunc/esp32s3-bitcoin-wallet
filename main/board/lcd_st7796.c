/*
 * Minimal ST7796 driver on top of esp_lcd panel IO (SPI).
 * Init sequence matches the panel used on the ONX3248G035.
 */
#include "lcd_st7796.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "board.h"

static const char *TAG = "st7796";

static esp_lcd_panel_io_handle_t s_io;
static lcd_flush_done_cb_t s_done_cb;
static void *s_done_user;

/* MADCTL: MX (column mirror) | BGR */
#define ST7796_MADCTL   0x48

typedef struct {
    uint8_t cmd;
    uint8_t data[14];
    uint8_t len;
    uint16_t delay_ms;
} init_cmd_t;

static const init_cmd_t s_init[] = {
    {0x11, {0}, 0, 120},                                   /* sleep out */
    {0x36, {ST7796_MADCTL}, 1, 0},
    {0x3A, {0x05}, 1, 0},                                  /* RGB565 */
    {0xF0, {0xC3}, 1, 0},                                  /* command set ctrl: enable part 1 */
    {0xF0, {0x96}, 1, 0},                                  /* enable part 2 */
    {0xB4, {0x01}, 1, 0},                                  /* 1-dot inversion */
    {0xB7, {0xC6}, 1, 0},
    {0xC0, {0x80, 0x45}, 2, 0},
    {0xC1, {0x13}, 1, 0},
    {0xC2, {0xA7}, 1, 0},
    {0xC5, {0x0A}, 1, 0},
    {0xE8, {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 8, 0},
    {0xE0, {0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30, 0x33, 0x47, 0x17, 0x13, 0x13, 0x2B, 0x31}, 14, 0},
    {0xE1, {0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x07, 0x2F, 0x33, 0x47, 0x38, 0x15, 0x16, 0x2C, 0x32}, 14, 0},
    {0xF0, {0x3C}, 1, 0},                                  /* disable part 1 */
    {0xF0, {0x69}, 1, 120},                                /* disable part 2 */
    {0x20, {0}, 0, 0},                                     /* inversion off */
    {0x29, {0}, 0, 20},                                    /* display on */
};

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    if (s_done_cb) {
        s_done_cb(s_done_user);
    }
    return false;
}

esp_err_t lcd_init(lcd_flush_done_cb_t done_cb, void *user)
{
    s_done_cb = done_cb;
    s_done_user = user;

    spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_SCLK,
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_H_RES * 80 * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_DC,
        .cs_gpio_num = BOARD_LCD_CS,
        .pclk_hz = BOARD_LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 8,
        .on_color_trans_done = on_trans_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &s_io),
                        TAG, "panel io");

    /* Hardware reset through the IO expander (also resets the touch controller). */
    board_exio_write(EXIO_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    board_exio_write(EXIO_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    esp_lcd_panel_io_tx_param(s_io, 0x01, NULL, 0);        /* software reset */
    vTaskDelay(pdMS_TO_TICKS(120));

    for (size_t i = 0; i < sizeof(s_init) / sizeof(s_init[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, s_init[i].cmd, s_init[i].data, s_init[i].len),
                            TAG, "init cmd 0x%02x", s_init[i].cmd);
        if (s_init[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(s_init[i].delay_ms));
        }
    }
    ESP_LOGI(TAG, "panel ready %dx%d", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return ESP_OK;
}

esp_err_t lcd_draw(int x1, int y1, int x2, int y2, const void *pixels)
{
    uint8_t caset[4] = { x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF };
    uint8_t raset[4] = { y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF };
    esp_lcd_panel_io_tx_param(s_io, 0x2A, caset, 4);
    esp_lcd_panel_io_tx_param(s_io, 0x2B, raset, 4);
    size_t len = (size_t)(x2 - x1 + 1) * (size_t)(y2 - y1 + 1) * 2;
    return esp_lcd_panel_io_tx_color(s_io, 0x2C, pixels, len);
}
