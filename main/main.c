#include <stdbool.h>

#include "button_manager.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_config.h"
#include "nvs_flash.h"
#include "status_led.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "[main]";

static void init_nvs(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS init requires erase: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "NVS initialized");
}

static void init_common_services(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(status_led_init());
    ESP_ERROR_CHECK(status_led_set_normal());
    ESP_ERROR_CHECK(button_manager_start());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(web_server_start());
}

static esp_err_t start_wifi_from_config(const wifi_cfg_t *cfg) {
    esp_err_t err = wifi_manager_start_sta(cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saved Wi-Fi failed, starting portal: %s",
                 esp_err_to_name(err));
        return wifi_manager_start_portal();
    }

    ESP_LOGI(TAG, "started with saved Wi-Fi configuration");
    return ESP_OK;
}

void app_main(void) {
    init_nvs();

    wifi_cfg_t saved_wifi_cfg;
    esp_err_t err = nvs_config_load_wifi(&saved_wifi_cfg);
    bool has_saved_wifi = err == ESP_OK;
    if (has_saved_wifi) {
        ESP_LOGI(TAG, "valid Wi-Fi config found in NVS");
    } else {
        ESP_LOGW(TAG, "no valid Wi-Fi config in NVS: %s", esp_err_to_name(err));
    }

    init_common_services();

    if (has_saved_wifi) {
        err = start_wifi_from_config(&saved_wifi_cfg);
    } else {
        err = wifi_manager_start_portal();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi startup failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "application started");
}
