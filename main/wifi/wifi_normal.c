#include "wifi/wifi_internal.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"

static const char *TAG = "[wifi]";

static bool s_connecting;
static bool s_skip_disconnect_once;
static int s_retry_count;

static void retry_connection(void) {
    if (s_retry_count >= WIFI_CONNECT_MAX_RETRY) {
        ESP_LOGW(TAG, "normal STA connection retry limit reached");
        return;
    }

    s_retry_count++;
    ESP_LOGI(TAG, "retrying normal STA connection, attempt=%d",
             s_retry_count);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "normal STA retry failed: %s", esp_err_to_name(err));
    }
}

static void reconnect_sta(void) {
    ESP_LOGI(TAG, "reconnecting STA in normal mode");
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "normal reconnect failed: %s", esp_err_to_name(err));
    }
}

static void normal_sta_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;

    if (event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "normal STA started");
        return;
    }

    wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)
        event_data;
    ESP_LOGW(TAG, "normal STA disconnected, reason=%d",
             event ? event->reason : -1);
    wifi_core_handle_sta_disconnected();

    if (s_skip_disconnect_once) {
        s_skip_disconnect_once = false;
        return;
    }

    if (s_connecting) {
        retry_connection();
    } else {
        reconnect_sta();
    }
}

static void normal_ip_event_handler(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;
    (void)event_id;

    s_retry_count = 0;
    s_skip_disconnect_once = false;
    wifi_core_handle_sta_got_ip(event_data);
}

static esp_err_t init_normal_wifi(void) {
    esp_err_t err = wifi_core_init_common_resources();
    if (err != ESP_OK) {
        return err;
    }

    err = wifi_core_init_driver();
    if (err != ESP_OK) {
        return err;
    }

    err = wifi_event_handler_register(normal_sta_event_handler,
                                          normal_ip_event_handler);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "wifi manager initialized in normal mode");
    return ESP_OK;
}

static esp_err_t connect_sta_blocking(wifi_config_t *wifi_config) {
    wifi_core_reset_sta_connection_result();
    s_connecting = true;
    s_retry_count = 0;
    s_skip_disconnect_once = wifi_core_disconnect_sta();

    esp_err_t err = wifi_core_start_sta_connect(wifi_config);
    if (err != ESP_OK) {
        s_connecting = false;
        s_skip_disconnect_once = false;
        return err;
    }

    err = wifi_core_wait_sta_connected(WIFI_CONNECT_TIMEOUT_MS);
    s_connecting = false;
    if (err != ESP_OK) {
        s_skip_disconnect_once = wifi_core_disconnect_sta();
        return err;
    }

    return ESP_OK;
}

static esp_err_t start_sta_mode(const wifi_cfg_t *cfg) {
    ESP_LOGI(TAG, "starting STA mode");

    wifi_config_t wifi_config;
    esp_err_t err =
        wifi_core_build_sta_config(&wifi_config, cfg->ssid, cfg->password);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set WIFI_MODE_STA failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_core_set_mode(WIFI_MODE_STA, NULL);

    err = connect_sta_blocking(&wifi_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saved Wi-Fi connection failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t wifi_manager_start_normal(const wifi_cfg_t *cfg) {
    esp_err_t err = init_normal_wifi();
    if (err != ESP_OK) {
        return err;
    }

    return start_sta_mode(cfg);
}
