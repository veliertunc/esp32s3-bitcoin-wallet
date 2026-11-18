idf_component_register(
    SRCS
        "main.c"
        "board/board.c"
        "board/lcd_st7796.c"
        "board/touch_cst826.c"
        "gui/lvgl_port.c"
        "gui/ui.c"
        "net/esplora.c"
        "net/wifi_mgr.c"
        "sys/sd_update.c"
        "sys/worker.c"
        "wallet/account.c"
        "wallet/btc_tx.c"
        "wallet/wallet.c"
    INCLUDE_DIRS "."
    REQUIRES
        trezor-crypto
        esp_driver_gpio esp_driver_i2c esp_driver_spi esp_driver_ledc esp_driver_sdmmc
        esp_adc esp_lcd esp_wifi esp_netif esp_event esp_http_client esp-tls mbedtls
        esp_timer esp_app_format app_update bootloader_support
        nvs_flash fatfs spiffs sdmmc vfs heap)
