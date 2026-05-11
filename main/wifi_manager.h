#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "nvs_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MANAGER_MODE_CONFIG = 0,
    WIFI_MANAGER_MODE_STA,
} wifi_manager_mode_t;

typedef struct {
    wifi_manager_mode_t mode;
    bool sta_connected;
    char sta_ip[16];
    bool ap_enabled;
    char ap_ssid[33];
} wifi_manager_status_t;

typedef struct {
    bool ok;
    char ip[16];
    esp_err_t err;
} wifi_manager_connect_result_t;

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_enter_config_mode(void);
esp_err_t wifi_manager_verify_and_save(const char *ssid,
                                        const char *password,
                                        wifi_manager_connect_result_t *result);
void wifi_manager_schedule_ap_shutdown(void);
void wifi_manager_get_status(wifi_manager_status_t *status);

#ifdef __cplusplus
}
#endif
