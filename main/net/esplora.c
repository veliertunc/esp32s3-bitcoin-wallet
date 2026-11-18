#include "esplora.h"

#include <stdlib.h>
#include <string.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "esplora";

#define MAX_BODY    (768 * 1024)

typedef struct {
    char  *buf;
    size_t len, cap;
    bool   overflow;
} rx_t;

struct esplora_s {
    esp_http_client_handle_t c;
    rx_t rx;
};

static esp_err_t on_event(esp_http_client_event_t *evt)
{
    rx_t *rx = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rx && evt->data_len > 0) {
        size_t need = rx->len + (size_t)evt->data_len + 1;
        if (need > MAX_BODY) {
            rx->overflow = true;
            return ESP_OK;
        }
        if (need > rx->cap) {
            size_t ncap = rx->cap ? rx->cap : 4096;
            while (ncap < need) {
                ncap *= 2;
            }
            char *nb = realloc(rx->buf, ncap);
            if (!nb) {
                rx->overflow = true;
                return ESP_OK;
            }
            rx->buf = nb;
            rx->cap = ncap;
        }
        memcpy(rx->buf + rx->len, evt->data, (size_t)evt->data_len);
        rx->len += (size_t)evt->data_len;
        rx->buf[rx->len] = 0;
    }
    return ESP_OK;
}

esplora_t *esplora_open(void)
{
    esplora_t *e = calloc(1, sizeof(*e));
    if (!e) {
        return NULL;
    }
    esp_http_client_config_t cfg = {
        .url = CONFIG_BTC_ESPLORA_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .event_handler = on_event,
        .user_data = &e->rx,
        .user_agent = "esp32-btc-wallet",
    };
    e->c = esp_http_client_init(&cfg);
    if (!e->c) {
        free(e);
        return NULL;
    }
    return e;
}

void esplora_close(esplora_t *e)
{
    if (!e) {
        return;
    }
    esp_http_client_cleanup(e->c);
    free(e->rx.buf);
    free(e);
}

static void rx_reset(rx_t *rx)
{
    free(rx->buf);
    memset(rx, 0, sizeof(*rx));
}

static esp_err_t do_request(esplora_t *e, const char *path, int *status)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", CONFIG_BTC_ESPLORA_URL, path);
    esp_http_client_set_url(e->c, url);
    rx_reset(&e->rx);
    esp_err_t err = esp_http_client_perform(e->c);
    if (err != ESP_OK) {
        /* connection may have been closed by the server - retry once */
        ESP_LOGW(TAG, "%s: %s, retrying", path, esp_err_to_name(err));
        rx_reset(&e->rx);
        esp_http_client_close(e->c);
        err = esp_http_client_perform(e->c);
    }
    *status = err == ESP_OK ? esp_http_client_get_status_code(e->c) : 0;
    if (err == ESP_OK && e->rx.overflow) {
        err = ESP_ERR_NO_MEM;
    }
    return err;
}

esp_err_t esplora_get(esplora_t *e, const char *path, char **body, size_t *len)
{
    int status = 0;
    esp_http_client_set_method(e->c, HTTP_METHOD_GET);
    esp_err_t err = do_request(e, path, &status);
    if (err == ESP_OK && status != 200) {
        ESP_LOGW(TAG, "GET %s -> HTTP %d", path, status);
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err != ESP_OK) {
        rx_reset(&e->rx);
        return err;
    }
    if (!e->rx.buf) {
        e->rx.buf = calloc(1, 1);
    }
    *body = e->rx.buf;
    if (len) {
        *len = e->rx.len;
    }
    memset(&e->rx, 0, sizeof(e->rx));   /* ownership moved to caller */
    return ESP_OK;
}

esp_err_t esplora_post(esplora_t *e, const char *path, const char *data, char **body, int *status)
{
    esp_http_client_set_method(e->c, HTTP_METHOD_POST);
    esp_http_client_set_header(e->c, "Content-Type", "text/plain");
    esp_http_client_set_post_field(e->c, data, (int)strlen(data));
    esp_err_t err = do_request(e, path, status);
    esp_http_client_set_post_field(e->c, NULL, 0);
    esp_http_client_delete_header(e->c, "Content-Type");
    esp_http_client_set_method(e->c, HTTP_METHOD_GET);
    if (err != ESP_OK) {
        rx_reset(&e->rx);
        return err;
    }
    *body = e->rx.buf ? e->rx.buf : calloc(1, 1);
    memset(&e->rx, 0, sizeof(e->rx));
    return ESP_OK;
}
