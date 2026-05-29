#include "button_manager.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "main";

static esp_err_t init_nvs(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS init requires erase: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "NVS initialized");
    return ESP_OK;
}

void app_main(void) {
    esp_err_t err = init_nvs();
    if (err != ESP_OK) {
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    err = button_manager_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button_manager_start failed: %s", esp_err_to_name(err));
        return;
    }

    err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_manager_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = web_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web_server_start failed: %s", esp_err_to_name(err));
        return;
    }

    err = wifi_manager_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_manager_start failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "application started");
}
