#include "wifi_mgr.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "wifi";

#define BIT_CONNECTED   BIT0
#define BIT_FAILED      BIT1
#define MAX_RETRY       5

static EventGroupHandle_t s_ev;
static volatile wifi_mgr_status_t s_status = WIFI_MGR_OFF;
static bool s_started;
static int s_retry;
static char s_ssid[33];

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, BIT_CONNECTED);
        if (s_status == WIFI_MGR_OFF) {
            return;
        }
        if (s_retry++ < MAX_RETRY) {
            s_status = WIFI_MGR_CONNECTING;
            esp_wifi_connect();
        } else {
            s_status = WIFI_MGR_FAILED;
            xEventGroupSetBits(s_ev, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        s_status = WIFI_MGR_CONNECTED;
        xEventGroupClearBits(s_ev, BIT_FAILED);
        xEventGroupSetBits(s_ev, BIT_CONNECTED);
    }
}

static bool load_creds(char *ssid, char *pass)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t l1 = 33, l2 = 65;
    bool ok = nvs_get_str(h, "ssid", ssid, &l1) == ESP_OK && nvs_get_str(h, "pass", pass, &l2) == ESP_OK;
    nvs_close(h);
    return ok && ssid[0];
}

static void save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass);
        nvs_commit(h);
        nvs_close(h);
    }
}

void wifi_mgr_forget(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_status = WIFI_MGR_OFF;
    esp_wifi_disconnect();
    s_ssid[0] = 0;
}

esp_err_t wifi_mgr_init(void)
{
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;

    char ssid[33] = {0}, pass[65] = {0};
    if (load_creds(ssid, pass)) {
        wifi_mgr_connect(ssid, pass, false);
    }
    return ESP_OK;
}

esp_err_t wifi_mgr_connect(const char *ssid, const char *pass, bool save)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;

    s_status = WIFI_MGR_OFF;           /* suppress auto-retry of the previous network */
    esp_wifi_disconnect();
    xEventGroupClearBits(s_ev, BIT_CONNECTED | BIT_FAILED);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    s_retry = 0;
    s_status = WIFI_MGR_CONNECTING;
    if (save) {
        save_creds(ssid, pass);
    }
    return esp_wifi_connect();
}

int wifi_mgr_scan(wifi_mgr_ap_t *out, int max)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    bool was_connecting = s_status == WIFI_MGR_CONNECTING;
    if (was_connecting) {
        /* a pending connect blocks scanning */
        s_status = WIFI_MGR_OFF;
        esp_wifi_disconnect();
    }
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return 0;
    }
    uint16_t n = 20;
    wifi_ap_record_t recs[20];
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        return 0;
    }
    int cnt = 0;
    for (int i = 0; i < n && cnt < max; i++) {
        if (!recs[i].ssid[0]) {
            continue;
        }
        bool dup = false;
        for (int k = 0; k < cnt; k++) {
            if (strcmp(out[k].ssid, (char *)recs[i].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        strlcpy(out[cnt].ssid, (char *)recs[i].ssid, sizeof(out[cnt].ssid));
        out[cnt].rssi = recs[i].rssi;
        out[cnt].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        cnt++;
    }
    return cnt;
}

wifi_mgr_status_t wifi_mgr_status(void)
{
    return s_status;
}

bool wifi_mgr_radio_on(void)
{
    return s_started;
}

bool wifi_mgr_wait_connected(int timeout_ms)
{
    if (!s_ev) {
        return false;
    }
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(timeout_ms));
    return (b & BIT_CONNECTED) != 0;
}

const char *wifi_mgr_ssid(void)
{
    return s_ssid;
}
