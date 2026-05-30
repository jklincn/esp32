#include <stdbool.h>

#include "app_nvs.h"
#include "button_manager.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "status_led.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "[main]";

typedef enum {
    STARTUP_MODE_SAVED_WIFI,
    STARTUP_MODE_PORTAL,
    STARTUP_MODE_ERROR,
} startup_mode_t;

static startup_mode_t select_startup_mode(wifi_cfg_t *saved_wifi_cfg) {
    esp_err_t err = app_nvs_load_wifi(saved_wifi_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "valid Wi-Fi config found in NVS");
        return STARTUP_MODE_SAVED_WIFI;
    }

    if (app_nvs_is_wifi_config_unavailable(err)) {
        ESP_LOGW(TAG, "no valid Wi-Fi config in NVS: %s", esp_err_to_name(err));
        return STARTUP_MODE_PORTAL;
    }

    ESP_LOGE(TAG, "failed to determine startup mode: %s", esp_err_to_name(err));
    return STARTUP_MODE_ERROR;
}

static void init_common_services(void) {
    ESP_ERROR_CHECK(app_nvs_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(status_led_init());
    ESP_ERROR_CHECK(button_manager_start());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(web_server_start());
}

static esp_err_t start_wifi_from_config(const wifi_cfg_t *cfg) {
    esp_err_t err = wifi_manager_start_sta(cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saved Wi-Fi failed: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(status_led_set_error());
        return err;
    }

    ESP_ERROR_CHECK(status_led_set_normal_blinking(false));
    ESP_LOGI(TAG, "started with saved Wi-Fi configuration");
    return ESP_OK;
}

void app_main(void) {
    init_common_services();

    wifi_cfg_t saved_wifi_cfg;
    startup_mode_t startup_mode = select_startup_mode(&saved_wifi_cfg);
    if (startup_mode == STARTUP_MODE_ERROR) {
        return;
    }

    esp_err_t err;
    switch (startup_mode) {
        case STARTUP_MODE_SAVED_WIFI:
            err = start_wifi_from_config(&saved_wifi_cfg);
            break;
        case STARTUP_MODE_PORTAL:
            ESP_ERROR_CHECK(status_led_set_normal_blinking(true));
            err = wifi_manager_start_portal();
            break;
        default:
            return;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi startup failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "application started");
}
