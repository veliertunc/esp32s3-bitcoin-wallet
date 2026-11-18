#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    WIFI_MGR_OFF = 0,
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED,
    WIFI_MGR_FAILED,
} wifi_mgr_status_t;

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    bool    secure;
} wifi_mgr_ap_t;

esp_err_t wifi_mgr_init(void);                      /* starts STA, auto-connects saved network */
esp_err_t wifi_mgr_connect(const char *ssid, const char *pass, bool save);
int       wifi_mgr_scan(wifi_mgr_ap_t *out, int max); /* blocking */
wifi_mgr_status_t wifi_mgr_status(void);
bool      wifi_mgr_radio_on(void);
bool      wifi_mgr_wait_connected(int timeout_ms);
const char *wifi_mgr_ssid(void);
void      wifi_mgr_forget(void);
