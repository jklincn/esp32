#include "client/client.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "client_config.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi/wifi.h"

#define SERVER_CLIENT_TASK_STACK 8192
#define SERVER_CLIENT_TASK_PRIORITY 4
#define SERVER_CLIENT_TIMEOUT_MS 8000
#define SERVER_HEARTBEAT_INTERVAL_MS 5000
#define SERVER_HEARTBEAT_INITIAL_DELAY_MS 1000
#define SERVER_API_URL_MAX_LEN 256

#define SERVER_MESSAGES_PATH "/messages"
#define SERVER_HEARTBEAT_PATH "/devices/heartbeat"

static const char *TAG = "[client]";

esp_err_t client_check_configured(void) {
    if (SMS_GATEWAY_API_URL[0] != '\0' && SMS_GATEWAY_API_TOKEN[0] != '\0') {
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t build_device_id(char *device_id, size_t device_id_size) {
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read STA MAC failed: %s", esp_err_to_name(err));
        return err;
    }

    int written = snprintf(device_id, device_id_size,
                           "esp32c6-%02X%02X%02X%02X%02X%02X", mac[0], mac[1],
                           mac[2], mac[3], mac[4], mac[5]);
    if (written < 0 || (size_t)written >= device_id_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t build_api_url(const char *path, char *url, size_t url_size) {
    size_t base_len = strlen(SMS_GATEWAY_API_URL);
    while (base_len > 0 && SMS_GATEWAY_API_URL[base_len - 1] == '/') {
        base_len--;
    }

    size_t path_len = strlen(path);
    if (base_len == 0 || path[0] != '/' || base_len + path_len + 1 > url_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(url, SMS_GATEWAY_API_URL, base_len);
    memcpy(url + base_len, path, path_len + 1);
    return ESP_OK;
}

static esp_err_t build_heartbeat_payload(char *payload, size_t payload_size) {
    char device_id[sizeof("esp32c6-FFFFFFFFFFFF")] = {0};
    esp_err_t err = build_device_id(device_id, sizeof(device_id));
    if (err != ESP_OK) {
        return err;
    }

    int written = snprintf(payload, payload_size,
                           "{\"deviceId\":\"%s\",\"smsReady\":false}",
                           device_id);
    if (written < 0 || (size_t)written >= payload_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t send_json_post(const char *url, const char *payload,
                                const char *description) {
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = SERVER_CLIENT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "init HTTP client failed");
        return ESP_ERR_NO_MEM;
    }

    char auth_header[sizeof("Bearer ") + sizeof(SMS_GATEWAY_API_TOKEN)];
    int written = snprintf(auth_header, sizeof(auth_header), "Bearer %s",
                           SMS_GATEWAY_API_TOKEN);
    if (written < 0 || (size_t)written >= sizeof(auth_header)) {
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_http_client_set_header(client, "Content-Type",
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

    ESP_LOGI(TAG, "posting %s to %s", description, url);
    err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s POST failed: %s", description, esp_err_to_name(err));
        return err;
    }

    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "%s POST returned HTTP %d", description, status_code);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "%s POST sent, status=%d", description, status_code);
    return ESP_OK;
}

static esp_err_t send_heartbeat_post(void) {
    char url[SERVER_API_URL_MAX_LEN] = {0};
    esp_err_t err = build_api_url(SERVER_HEARTBEAT_PATH, url, sizeof(url));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "build heartbeat URL failed: %s", esp_err_to_name(err));
        return err;
    }

    char payload[128] = {0};
    err = build_heartbeat_payload(payload, sizeof(payload));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "build heartbeat payload failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    return send_json_post(url, payload, "heartbeat");
}

static void server_heartbeat_task(void *arg) {
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(SERVER_HEARTBEAT_INITIAL_DELAY_MS));

    while (true) {
        wifi_manager_status_t status;
        wifi_manager_get_status(&status);

        if (status.sta_connected) {
            esp_err_t err = send_heartbeat_post();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "heartbeat not sent: %s", esp_err_to_name(err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SERVER_HEARTBEAT_INTERVAL_MS));
    }
}

esp_err_t client_start_heartbeat(void) {
    BaseType_t ok = xTaskCreate(server_heartbeat_task, "server_heartbeat",
                                SERVER_CLIENT_TASK_STACK, NULL,
                                SERVER_CLIENT_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create heartbeat task failed");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
