#include "notify/notify.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "notify_config.h"
#include "wifi/wifi.h"

#define SERVER_NOTIFY_TASK_STACK 8192
#define SERVER_NOTIFY_TASK_PRIORITY 4
#define SERVER_NOTIFY_TIMEOUT_MS 8000

static const char *TAG = "[notify]";

static bool is_configured(void) {
    return SERVER_NOTIFY_URL[0] != '\0' && SERVER_NOTIFY_API_TOKEN[0] != '\0';
}

static esp_err_t build_device_id(char *device_id, size_t device_id_size) {
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read STA MAC failed: %s", esp_err_to_name(err));
        return err;
    }

    int written = snprintf(device_id, device_id_size,
                           "esp32c6-%02X%02X%02X%02X%02X%02X", mac[0],
                           mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (written < 0 || (size_t)written >= device_id_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t build_payload(char *payload, size_t payload_size) {
    char device_id[sizeof("esp32c6-FFFFFFFFFFFF")] = {0};
    esp_err_t err = build_device_id(device_id, sizeof(device_id));
    if (err != ESP_OK) {
        return err;
    }

    wifi_manager_status_t status;
    wifi_manager_get_status(&status);

    int written = snprintf(
        payload, payload_size,
        "{\"senderNumber\":\"system\","
        "\"senderName\":\"SMS Gateway\","
        "\"body\":\"SMS Gateway connected to Wi-Fi\","
        "\"deviceId\":\"%s\","
        "\"rawPayload\":{\"event\":\"startup_connected\",\"ip\":\"%s\"}}",
        device_id, status.sta_ip);
    if (written < 0 || (size_t)written >= payload_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t send_startup_post(void) {
    char payload[384] = {0};
    esp_err_t err = build_payload(payload, sizeof(payload));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "build startup payload failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_http_client_config_t config = {
        .url = SERVER_NOTIFY_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = SERVER_NOTIFY_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "init HTTP client failed");
        return ESP_ERR_NO_MEM;
    }

    char auth_header[sizeof("Bearer ") + sizeof(SERVER_NOTIFY_API_TOKEN)];
    int written = snprintf(auth_header, sizeof(auth_header), "Bearer %s",
                           SERVER_NOTIFY_API_TOKEN);
    if (written < 0 || (size_t)written >= sizeof(auth_header)) {
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    err = esp_http_client_set_header(client, "Content-Type",
                                     "application/json");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set Content-Type header failed: %s",
                 esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    err = esp_http_client_set_header(client, "Authorization", auth_header);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set Authorization header failed: %s",
                 esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    err = esp_http_client_set_post_field(client, payload, strlen(payload));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set POST body failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    ESP_LOGI(TAG, "posting startup notification to %s", SERVER_NOTIFY_URL);
    err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "startup POST failed: %s", esp_err_to_name(err));
        return err;
    }

    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "startup POST returned HTTP %d", status_code);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "startup POST sent, status=%d", status_code);
    return ESP_OK;
}

static void server_notify_task(void *arg) {
    (void)arg;

    esp_err_t err = send_startup_post();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "startup notification not sent: %s",
                 esp_err_to_name(err));
    }

    vTaskDelete(NULL);
}

esp_err_t server_notify_startup_connected(void) {
    if (!is_configured()) {
        ESP_LOGW(TAG, "startup POST skipped because URL or token is empty");
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(server_notify_task, "server_notify",
                                SERVER_NOTIFY_TASK_STACK, NULL,
                                SERVER_NOTIFY_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create startup notification task failed");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
