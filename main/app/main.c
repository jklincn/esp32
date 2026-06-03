#include <stdbool.h>

#include "button/button.h"
#include "client/client.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "led/led.h"
#include "storage/storage.h"
#include "web/web.h"
#include "wifi/wifi.h"

static const char *TAG = "[main]";

typedef enum {
    STARTUP_MODE_NORMAL,
    STARTUP_MODE_CONFIG,
    STARTUP_MODE_ERROR,
} startup_mode_t;

static startup_mode_t select_startup_mode(void) {
    esp_err_t err = app_nvs_check_wifi_config();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "valid Wi-Fi config found in NVS");
        return STARTUP_MODE_NORMAL;
    }

    if (app_nvs_is_wifi_config_unavailable(err)) {
        ESP_LOGW(TAG, "no valid Wi-Fi config in NVS: %s", esp_err_to_name(err));
        return STARTUP_MODE_CONFIG;
    }

    ESP_LOGE(TAG, "failed to determine startup mode: %s", esp_err_to_name(err));
    return STARTUP_MODE_ERROR;
}

static esp_err_t init_common_services(void) {
    esp_err_t err = app_nvs_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = system_led_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "system LED init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = button_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button init failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static esp_err_t normal_mode(void) {
    esp_err_t err = client_check_configured();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SMS Gateway client config invalid: %s",
                 esp_err_to_name(err));
        return err;
    }

    wifi_cfg_t saved_wifi_cfg;
    err = app_nvs_load_wifi(&saved_wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load saved Wi-Fi config failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = wifi_start_normal(&saved_wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saved Wi-Fi failed: %s", esp_err_to_name(err));
        return err;
    }

    err = web_server_start(WEB_SERVER_MODE_NORMAL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "normal web server start failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = system_led_set(SYSTEM_LED_EFFECT_GREEN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set normal LED failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "started with saved Wi-Fi configuration");

    err = client_start_heartbeat();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start heartbeat task failed: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

static esp_err_t config_mode(void) {
    esp_err_t err = wifi_start_config();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config Wi-Fi failed: %s", esp_err_to_name(err));
        return err;
    }

    err = web_server_start(WEB_SERVER_MODE_CONFIG);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config web server start failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = system_led_set(SYSTEM_LED_EFFECT_BLUE_BLINK);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set config LED failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "started in portal mode");
    return ESP_OK;
}

void app_main(void) {
    esp_err_t err = init_common_services();
    if (err != ESP_OK) {
        return;
    }

    startup_mode_t startup_mode = select_startup_mode();

    err = ESP_FAIL;
    switch (startup_mode) {
        case STARTUP_MODE_NORMAL:
            err = normal_mode();
            break;
        case STARTUP_MODE_CONFIG:
            err = config_mode();
            break;
        case STARTUP_MODE_ERROR:
            err = system_led_set(SYSTEM_LED_EFFECT_RED);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "set error LED failed: %s", esp_err_to_name(err));
            }
            return;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "application startup failed: %s", esp_err_to_name(err));
        esp_err_t led_err = system_led_set(SYSTEM_LED_EFFECT_RED);
        if (led_err != ESP_OK) {
            ESP_LOGE(TAG, "set error LED failed: %s", esp_err_to_name(led_err));
        }
        return;
    }

    ESP_LOGI(TAG, "application started");
}
