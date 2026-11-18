/*
 * SD-card Bitcoin wallet firmware for the ONX3248G035 (ESP32-S3 + 3.5" ST7796 + CST826).
 *
 *   - BIP39 / BIP32 / BIP84 (native segwit, P2WPKH) HD wallet
 *   - wallet secret encrypted (PBKDF2-HMAC-SHA512 + ChaCha20-Poly1305) on the microSD card
 *   - receive (QR), send (coin selection, BIP143 signing, RBF), history via Esplora over HTTPS
 *   - LVGL 9 touch UI, firmware update from /sdcard/firmware.bin
 */
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board/board.h"
#include "gui/lvgl_port.h"
#include "gui/ui.h"
#include "net/wifi_mgr.h"
#include "sys/sd_update.h"
#include "sys/worker.h"
#include "wallet/wallet.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NOTE: erasing NVS also erases the device-binding secret. Only happens
         * after a partition-layout change; the recovery phrase restores funds. */
        ESP_LOGW(TAG, "NVS reformat");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(board_init());
    board_spiffs_mount();
    board_sd_mount();                 /* failure is handled by the UI (retry screen) */

    /* Radio on early: also makes esp_random() a true RNG for key generation. */
    ESP_ERROR_CHECK(wifi_mgr_init());

    if (!wallet_selftest()) {
        ESP_LOGE(TAG, "crypto self-test failed - wallet functions disabled");
    }
    worker_start();
    ESP_ERROR_CHECK(lvgl_port_start());
    ui_start();
    board_backlight_set(80);

    sd_update_mark_valid();
    ESP_LOGI(TAG, "ready, free heap %u (internal %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
