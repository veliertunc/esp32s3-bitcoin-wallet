#include "lvgl_port.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board/board.h"
#include "board/lcd_st7796.h"
#include "board/touch_cst826.h"
#include "wallet/wallet.h"

static const char *TAG = "lvgl";

#define DRAW_LINES      60

static SemaphoreHandle_t s_lock;
static lv_display_t *s_disp;

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush_done(void *user)
{
    lv_display_flush_ready((lv_display_t *)user);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    uint32_t n = lv_area_get_width(area) * lv_area_get_height(area);
    lv_draw_sw_rgb565_swap(px, n);   /* SPI LCD expects big-endian RGB565 */
    lcd_draw(area->x1, area->y1, area->x2, area->y2, px);
}

static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x, y;
    if (touch_read(&x, &y)) {
        /* touch coordinates + timing feed the key-generation entropy pool */
        static uint16_t samples[32];
        static int n;
        samples[n++] = x ^ (uint16_t)(y << 7) ^ (uint16_t)esp_timer_get_time();
        if (n == 32) {
            wallet_entropy_add(samples, sizeof(samples));
            n = 0;
        }
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

bool ui_lock(int timeout_ms)
{
    TickType_t t = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lock, t) == pdTRUE;
}

void ui_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

static void lvgl_task(void *arg)
{
    for (;;) {
        uint32_t wait = 10;
        if (ui_lock(-1)) {
            wait = lv_timer_handler();
            ui_unlock();
        }
        if (wait < 5) {
            wait = 5;
        } else if (wait > 50) {
            wait = 50;
        }
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

esp_err_t lvgl_port_start(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();

    lv_init();
    lv_tick_set_cb(tick_cb);

    s_disp = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    ESP_ERROR_CHECK(lcd_init(flush_done, s_disp));

    size_t buf_bytes = BOARD_LCD_H_RES * DRAW_LINES * sizeof(uint16_t);
    void *b1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *b2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!b1 || !b2) {
        ESP_LOGE(TAG, "no DMA memory for draw buffers");
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, b1, b2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);

    if (touch_init() == ESP_OK) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_cb);
        lv_indev_set_display(indev, s_disp);
    }

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, NULL, 1);
    return ESP_OK;
}
