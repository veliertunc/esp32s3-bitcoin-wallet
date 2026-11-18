#include "sd_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sd_update";

bool sd_update_available(char *version, size_t version_len)
{
    FILE *f = fopen(SD_UPDATE_FILE, "rb");
    if (!f) {
        return false;
    }
    uint8_t hdr[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)];
    size_t n = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (n != sizeof(hdr) || hdr[0] != ESP_IMAGE_HEADER_MAGIC) {
        return false;
    }
    const esp_app_desc_t *d =
        (const esp_app_desc_t *)(hdr + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return false;
    }
    if (version) {
        snprintf(version, version_len, "%.32s (%.32s)", d->version, d->project_name);
    }
    return true;
}

esp_err_t sd_update_apply(sd_update_progress_cb_t cb)
{
    struct stat st;
    if (stat(SD_UPDATE_FILE, &st) != 0 || st.st_size <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((size_t)st.st_size > part->size) {
        ESP_LOGE(TAG, "image (%ld bytes) larger than slot (%lu)", (long)st.st_size, (unsigned long)part->size);
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *f = fopen(SD_UPDATE_FILE, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(part, (size_t)st.st_size, &h);
    if (err != ESP_OK) {
        fclose(f);
        return err;
    }
    const size_t chunk = 8192;
    uint8_t *buf = malloc(chunk);
    size_t done = 0;
    int last_pct = -1;
    while (err == ESP_OK && buf) {
        size_t r = fread(buf, 1, chunk, f);
        if (r == 0) {
            break;
        }
        err = esp_ota_write(h, buf, r);
        done += r;
        int pct = (int)(done * 100 / (size_t)st.st_size);
        if (cb && pct != last_pct) {
            cb(pct);
            last_pct = pct;
        }
    }
    free(buf);
    fclose(f);
    if (!buf) {
        err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        esp_ota_abort(h);
        return err;
    }
    err = esp_ota_end(h);           /* verifies image checksum/signature */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image validation failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        return err;
    }
    /* rename so we don't re-apply the same image after reboot */
    unlink(SD_UPDATE_FILE ".done");
    rename(SD_UPDATE_FILE, SD_UPDATE_FILE ".done");
    ESP_LOGI(TAG, "update written to %s, rebooting", part->label);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

void sd_update_mark_valid(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed");
    }
}
