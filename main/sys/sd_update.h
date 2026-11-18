/*
 * Firmware update from the microSD card.
 *
 * Copy a build (build/esp32s3-bitcoin-wallet.bin) to the card as
 *     /sdcard/firmware.bin
 * then choose Settings -> "Update firmware from SD". The image is written to
 * the inactive OTA slot, validated by esp_ota_end() (and by secure boot if
 * enabled), and the device reboots into it. If the new image fails to start
 * the bootloader rolls back automatically (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define SD_UPDATE_FILE  "/sdcard/firmware.bin"

typedef void (*sd_update_progress_cb_t)(int percent);

bool      sd_update_available(char *version, size_t version_len);
esp_err_t sd_update_apply(sd_update_progress_cb_t cb);   /* reboots on success */

/* Call once the UI is up: confirms the running image so it is not rolled back. */
void      sd_update_mark_valid(void);
