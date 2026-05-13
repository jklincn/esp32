#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define WIFI_CFG_NAMESPACE "wifi_cfg"
#define WIFI_CFG_KEY_SSID "ssid"
#define WIFI_CFG_KEY_PASSWORD "password"
#define WIFI_CFG_KEY_INITIALIZED "initialized"

#define WIFI_CFG_MAX_SSID_LEN 32
#define WIFI_CFG_MAX_PASSWORD_LEN 64

typedef struct {
    char ssid[WIFI_CFG_MAX_SSID_LEN + 1];
    char password[WIFI_CFG_MAX_PASSWORD_LEN + 1];
    bool initialized;
} wifi_cfg_t;

esp_err_t nvs_config_load_wifi(wifi_cfg_t *cfg);
esp_err_t nvs_config_save_wifi(const char *ssid, const char *password);
esp_err_t nvs_config_clear_wifi(void);
bool nvs_config_is_wifi_valid(const wifi_cfg_t *cfg);
