#pragma once

#include "wifi/wifi.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"

#define WIFI_CONNECT_TIMEOUT_MS 10000
#define WIFI_CONNECT_MAX_RETRY 5

esp_err_t wifi_core_init_common_resources(void);
esp_err_t wifi_core_init_driver(void);
esp_err_t wifi_core_ensure_started(void);
esp_err_t wifi_event_handler_register(esp_event_handler_t wifi_handler,
                                          esp_event_handler_t ip_handler);
esp_err_t wifi_core_build_sta_config(wifi_config_t *wifi_config,
                                     const char *ssid,
                                     const char *password);
void wifi_core_reset_sta_connection_result(void);
bool wifi_core_disconnect_sta(void);
esp_err_t wifi_core_start_sta_connect(wifi_config_t *wifi_config);
esp_err_t wifi_core_wait_sta_connected(int timeout_ms);
void wifi_core_handle_sta_disconnected(void);
void wifi_core_handle_sta_got_ip(void *event_data);
void wifi_core_set_mode(wifi_mode_t mode, const char *ap_ssid);
