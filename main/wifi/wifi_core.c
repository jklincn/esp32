#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "wifi/wifi_internal.h"

/* 事件组 bit：事件回调通过它们把异步连接结果通知给阻塞等待的连接流程。 */
#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "[wifi]";

static EventGroupHandle_t s_wifi_event_group;
static SemaphoreHandle_t s_state_lock;
static esp_netif_t *s_sta_netif;
static bool s_wifi_started;

/* 对外状态缓存。写入通过 wifi_core_set_*，读取通过 wifi_get_status()。 */
static wifi_status_t s_status = {
    .mode = WIFI_MODE_APSTA,
    .sta_connected = false,
    .sta_ip = "0.0.0.0",
    .ap_ssid = "",
};

/* 标记 STA 已拿到 IP，并保存文本形式的 IPv4 地址。 */
static void state_set_sta_connected(const char *ip) {
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_status.sta_connected = true;
    snprintf(s_status.sta_ip, sizeof(s_status.sta_ip), "%s", ip);
    xSemaphoreGive(s_state_lock);
}

/* 标记 STA 断开；IP 回到占位值，避免页面展示过期地址。 */
static void state_set_sta_disconnected(void) {
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_status.sta_connected = false;
    snprintf(s_status.sta_ip, sizeof(s_status.sta_ip), "0.0.0.0");
    xSemaphoreGive(s_state_lock);
}

void wifi_core_set_mode(wifi_mode_t mode, const char *ap_ssid) {
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_status.mode = mode;
    if (ap_ssid != NULL) {
        snprintf(s_status.ap_ssid, sizeof(s_status.ap_ssid), "%s", ap_ssid);
    } else if (mode == WIFI_MODE_STA) {
        s_status.ap_ssid[0] = '\0';
    }
    xSemaphoreGive(s_state_lock);
}

void wifi_core_handle_sta_disconnected(void) {
    state_set_sta_disconnected();
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
}

void wifi_core_handle_sta_got_ip(void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    char ip[16] = "0.0.0.0";
    esp_ip4addr_ntoa(&event->ip_info.ip, ip, sizeof(ip));
    ESP_LOGI(TAG, "STA got IP: %s", ip);
    state_set_sta_connected(ip);
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
}

static bool is_valid_sta_credentials(const char *ssid, const char *password) {
    return ssid != NULL && password != NULL && ssid[0] != '\0' &&
           strlen(ssid) <= WIFI_CFG_MAX_SSID_LEN &&
           strlen(password) <= WIFI_CFG_MAX_PASSWORD_LEN;
}

esp_err_t wifi_core_build_sta_config(wifi_config_t *wifi_config,
                                     const char *ssid, const char *password) {
    if (!is_valid_sta_credentials(ssid, password)) {
        ESP_LOGE(TAG, "invalid STA credentials");
        return ESP_ERR_INVALID_ARG;
    }

    memset(wifi_config, 0, sizeof(*wifi_config));
    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);
    memcpy(wifi_config->sta.ssid, ssid, ssid_len);
    memcpy(wifi_config->sta.password, password, password_len);
    wifi_config->sta.threshold.authmode = password_len == 0
                                              ? WIFI_AUTH_OPEN
                                              : WIFI_AUTH_WPA2_PSK;
    wifi_config->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    return ESP_OK;
}

esp_err_t wifi_core_ensure_started(void) {
    if (s_wifi_started) {
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_wifi_started = true;
    return ESP_OK;
}

void wifi_core_reset_sta_connection_result(void) {
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    state_set_sta_disconnected();
}

bool wifi_core_disconnect_sta(void) {
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED &&
        err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "disconnect STA failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

esp_err_t wifi_core_start_sta_connect(wifi_config_t *wifi_config) {
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set STA config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = wifi_core_ensure_started();
    if (err != ESP_OK) {
        return err;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    ESP_LOGI(TAG, "connecting STA to ssid=%s", wifi_config->sta.ssid);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t wifi_core_wait_sta_connected(int timeout_ms) {
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "STA connected");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "STA connection timed out");
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_core_init_common_resources(void) {
    s_wifi_event_group = xEventGroupCreate();
    s_state_lock = xSemaphoreCreateMutex();
    if (s_wifi_event_group == NULL || s_state_lock == NULL) {
        ESP_LOGE(TAG, "failed to create synchronization primitives");
        return ESP_ERR_NO_MEM;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "failed to create default STA netif");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t wifi_core_init_driver(void) {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // 使用 RAM 存储 Wi-Fi 配置，避免 esp_wifi_set_config() 自动写入系统 Wi-Fi
    // NVS
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set Wi-Fi storage RAM failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t wifi_event_handler_register(esp_event_handler_t wifi_handler,
                                      esp_event_handler_t ip_handler) {
    esp_err_t err = esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_START, wifi_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register STA_START handler failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register STA_DISCONNECTED handler failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              ip_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register STA_GOT_IP handler failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

void wifi_get_status(wifi_status_t *status) {
    /* 用互斥锁保护结构体整体复制，避免 Web API 读到半更新状态。 */
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_state_lock);
}
