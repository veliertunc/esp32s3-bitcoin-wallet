#include "board.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_exio;
static uint8_t s_exio_state = 0xFF;
static SemaphoreHandle_t s_exio_mtx;

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static SemaphoreHandle_t s_adc_mtx;
static adc_channel_t s_bat_ch;

static sdmmc_card_t *s_card;

i2c_master_bus_handle_t board_i2c_bus(void) { return s_i2c_bus; }

esp_err_t board_exio_write(int bit, bool level)
{
    xSemaphoreTake(s_exio_mtx, portMAX_DELAY);
    if (level) {
        s_exio_state |= (uint8_t)(1u << bit);
    } else {
        s_exio_state &= (uint8_t)~(1u << bit);
    }
    esp_err_t err = i2c_master_transmit(s_exio, &s_exio_state, 1, 100);
    xSemaphoreGive(s_exio_mtx);
    return err;
}

esp_err_t board_exio_read(uint8_t *port)
{
    xSemaphoreTake(s_exio_mtx, portMAX_DELAY);
    esp_err_t err = i2c_master_receive(s_exio, port, 1, 100);
    xSemaphoreGive(s_exio_mtx);
    return err;
}

static esp_err_t i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BOARD_I2C_PORT,
        .scl_io_num = BOARD_I2C_SCL,
        .sda_io_num = BOARD_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c_bus), TAG, "i2c bus");

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_PCF8574_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev, &s_exio), TAG, "pcf8574");
    s_exio_mtx = xSemaphoreCreateMutex();

    /* All pins high: SD CS/DAT3 high (SD mode), LCD out of reset, inputs released. */
    s_exio_state = 0xFF;
    return i2c_master_transmit(s_exio, &s_exio_state, 1, 100);
}

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_1,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 10000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_1,
        .gpio_num = BOARD_LCD_BL,
        .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

void board_backlight_set(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (1023u * percent) / 100u;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void battery_init(void)
{
    s_adc_mtx = xSemaphoreCreateMutex();
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(BOARD_BAT_ADC_GPIO, &unit, &s_bat_ch) != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC pin not usable");
        return;
    }
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) {
        s_adc = NULL;
        return;
    }
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(s_adc, s_bat_ch, &ccfg);

    adc_cali_curve_fitting_config_t cal = {
        .unit_id = unit, .chan = s_bat_ch, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_adc_cali) != ESP_OK) {
        s_adc_cali = NULL;
    }
}

void board_adc_lock(void)
{
    if (s_adc_mtx) {
        xSemaphoreTake(s_adc_mtx, portMAX_DELAY);
    }
}

void board_adc_unlock(void)
{
    if (s_adc_mtx) {
        xSemaphoreGive(s_adc_mtx);
    }
}

int board_battery_mv(void)
{
    if (!s_adc) {
        return 0;
    }
    int sum = 0, n = 0;
    board_adc_lock();
    for (int i = 0; i < 8; i++) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(s_adc, s_bat_ch, &raw) != ESP_OK) {
            continue;
        }
        if (s_adc_cali && adc_cali_raw_to_voltage(s_adc_cali, raw, &mv) == ESP_OK) {
            sum += mv;
        } else {
            sum += raw * 3100 / 4095;
        }
        n++;
    }
    board_adc_unlock();
    if (!n) {
        return 0;
    }
    /* divider: 1M (top) / 2.2M (bottom) */
    return (sum / n) * 32 / 22;
}

int board_battery_percent(int mv)
{
    if (mv <= 0) {
        return -1;
    }
    if (mv >= 4150) {
        return 100;
    }
    if (mv <= 3300) {
        return 0;
    }
    return (mv - 3300) * 100 / (4150 - 3300);
}

bool board_is_charging(void)
{
    uint8_t p = 0xFF;
    if (board_exio_read(&p) != ESP_OK) {
        return false;
    }
    return !(p & (1u << EXIO_CHG_N));
}

esp_err_t board_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "i2c");
    backlight_init();
    battery_init();
    return ESP_OK;
}

/* ------------------------------------------------------------------ SD */

esp_err_t board_sd_mount(void)
{
    if (s_card) {
        return ESP_OK;
    }
    /* DAT3/CS must be high so the card powers up in SD (not SPI) mode. */
    board_exio_write(EXIO_SD_CS, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = BOARD_SD_CLK;
    slot.cmd = BOARD_SD_CMD;
    slot.d0 = BOARD_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        s_card = NULL;
        return err;
    }
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

void board_sd_unmount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
}

bool board_sd_mounted(void)
{
    return s_card != NULL;
}

esp_err_t board_spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_MOUNT_POINT,
        .partition_label = "spiffs",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
    }
    return err;
}
