#include "wifi_manager.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_config.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_CONNECT_MAX_RETRY 5
#define WIFI_AP_PASSWORD "12345678"
#define WIFI_AP_CHANNEL 1
#define WIFI_AP_MAX_CONN 4

static const char *TAG = "wifi_manager";

static EventGroupHandle_t s_wifi_event_group;
static SemaphoreHandle_t s_connect_lock;
static SemaphoreHandle_t s_state_lock;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static bool s_wifi_started;
static bool s_connecting;
static int s_retry_count;
static wifi_manager_status_t s_status = {
    .mode = WIFI_MANAGER_MODE_CONFIG,
    .sta_connected = false,
    .sta_ip = "0.0.0.0",
    .ap_enabled = false,
    .ap_ssid = "",
};

static void state_set_sta_connected(const char *ip)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_status.sta_connected = true;
    snprintf(s_status.sta_ip, sizeof(s_status.sta_ip), "%s", ip);
    xSemaphoreGive(s_state_lock);
}

static void state_set_sta_disconnected(void)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_status.sta_connected = false;
    snprintf(s_status.sta_ip, sizeof(s_status.sta_ip), "0.0.0.0");
    xSemaphoreGive(s_state_lock);
}

static void state_set_mode(wifi_manager_mode_t mode, bool ap_enabled, const char *ap_ssid)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_status.mode = mode;
    s_status.ap_enabled = ap_enabled;
    if (ap_ssid != NULL) {
        snprintf(s_status.ap_ssid, sizeof(s_status.ap_ssid), "%s", ap_ssid);
    } else if (!ap_enabled) {
        s_status.ap_ssid[0] = '\0';
    }
    xSemaphoreGive(s_state_lock);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started");
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "STA disconnected, reason=%d", event ? event->reason : -1);
        state_set_sta_disconnected();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        if (s_connecting) {
            if (s_retry_count < WIFI_CONNECT_MAX_RETRY) {
                s_retry_count++;
                ESP_LOGI(TAG, "retrying STA connection, attempt=%d", s_retry_count);
                esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "esp_wifi_connect retry failed: %s", esp_err_to_name(err));
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
            } else {
                ESP_LOGW(TAG, "STA connection failed after retries");
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        } else {
            wifi_manager_status_t status;
            wifi_manager_get_status(&status);
            if (status.mode == WIFI_MANAGER_MODE_STA) {
                ESP_LOGI(TAG, "reconnecting STA in normal mode");
                esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "normal reconnect failed: %s", esp_err_to_name(err));
                }
            }
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "SoftAP started");
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        ESP_LOGI(TAG, "SoftAP stopped");
        state_set_mode(WIFI_MANAGER_MODE_STA, false, NULL);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip[16] = "0.0.0.0";
        esp_ip4addr_ntoa(&event->ip_info.ip, ip, sizeof(ip));
        ESP_LOGI(TAG, "STA got IP: %s", ip);
        s_retry_count = 0;
        state_set_sta_connected(ip);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t configure_ap_ip(void)
{
    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "stop DHCP server failed: %s", esp_err_to_name(err));
    }

    esp_netif_ip_info_t ip_info = {
        .ip.addr = ESP_IP4TOADDR(192, 168, 4, 1),
        .gw.addr = ESP_IP4TOADDR(192, 168, 4, 1),
        .netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0),
    };

    err = esp_netif_set_ip_info(s_ap_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set SoftAP IP failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_dhcps_start(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGE(TAG, "start DHCP server failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SoftAP IP configured as 192.168.4.1");
    return ESP_OK;
}

static void fill_sta_config(wifi_config_t *wifi_config, const char *ssid, const char *password)
{
    memset(wifi_config, 0, sizeof(*wifi_config));
    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);
    memcpy(wifi_config->sta.ssid, ssid, ssid_len);
    memcpy(wifi_config->sta.password, password, password_len);
    wifi_config->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
}

static esp_err_t ensure_wifi_started(void)
{
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

static esp_err_t connect_sta_blocking(const char *ssid, const char *password, int timeout_ms)
{
    if (ssid == NULL || password == NULL ||
        ssid[0] == '\0' || password[0] == '\0' ||
        strlen(ssid) > WIFI_CFG_MAX_SSID_LEN ||
        strlen(password) > WIFI_CFG_MAX_PASSWORD_LEN) {
        ESP_LOGE(TAG, "invalid STA credentials");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config;
    fill_sta_config(&wifi_config, ssid, password);

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    state_set_sta_disconnected();
    s_retry_count = 0;
    s_connecting = true;

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "disconnect before connect failed: %s", esp_err_to_name(err));
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set STA config failed: %s", esp_err_to_name(err));
        s_connecting = false;
        return err;
    }

    err = ensure_wifi_started();
    if (err != ESP_OK) {
        s_connecting = false;
        return err;
    }

    ESP_LOGI(TAG, "connecting STA to ssid=%s", ssid);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        s_connecting = false;
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    s_connecting = false;

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "STA connected");
        return ESP_OK;
    }

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "STA connection failed");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "STA connection timed out");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t start_sta_mode(const wifi_cfg_t *cfg)
{
    ESP_LOGI(TAG, "starting STA mode");

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set WIFI_MODE_STA failed: %s", esp_err_to_name(err));
        return err;
    }

    state_set_mode(WIFI_MANAGER_MODE_STA, false, NULL);

    err = connect_sta_blocking(cfg->ssid, cfg->password, WIFI_CONNECT_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saved Wi-Fi connection failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t wifi_manager_init(void)
{
    if (s_wifi_event_group != NULL) {
        return ESP_OK;
    }

    s_wifi_event_group = xEventGroupCreate();
    s_connect_lock = xSemaphoreCreateMutex();
    s_state_lock = xSemaphoreCreateMutex();
    if (s_wifi_event_group == NULL || s_connect_lock == NULL || s_state_lock == NULL) {
        ESP_LOGE(TAG, "failed to create synchronization primitives");
        return ESP_ERR_NO_MEM;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) {
        ESP_LOGE(TAG, "failed to create default Wi-Fi netifs");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = configure_ap_ip();
    if (err != ESP_OK) {
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set Wi-Fi storage RAM failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "disable Wi-Fi power save failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi power save disabled for mDNS reliability");
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register WIFI_EVENT handler failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register IP_EVENT handler failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "wifi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_manager_enter_config_mode(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read SoftAP MAC failed: %s", esp_err_to_name(err));
        return err;
    }

    char ap_ssid[sizeof("SMS-Gateway-FFFF")];
    snprintf(ap_ssid, sizeof(ap_ssid), "SMS-Gateway-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_config = {0};
    ap_config.ap.ssid_len = strlen(ap_ssid);
    memcpy(ap_config.ap.ssid, ap_ssid, ap_config.ap.ssid_len);
    snprintf((char *)ap_config.ap.password, sizeof(ap_config.ap.password), "%s", WIFI_AP_PASSWORD);
    ap_config.ap.channel = WIFI_AP_CHANNEL;
    ap_config.ap.max_connection = WIFI_AP_MAX_CONN;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.pmf_cfg.required = false;

    ESP_LOGI(TAG, "starting config mode, SoftAP SSID=%s", ap_ssid);

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set WIFI_MODE_APSTA failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set SoftAP config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = ensure_wifi_started();
    if (err != ESP_OK) {
        return err;
    }

    state_set_mode(WIFI_MANAGER_MODE_CONFIG, true, ap_ssid);
    ESP_LOGI(TAG, "config mode ready at http://192.168.4.1");
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    wifi_cfg_t cfg;
    esp_err_t err = nvs_config_load_wifi(&cfg);
    if (err == ESP_OK && nvs_config_is_wifi_valid(&cfg)) {
        err = start_sta_mode(&cfg);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "started with saved STA configuration");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "falling back to config mode");
    } else {
        ESP_LOGW(TAG, "no valid saved Wi-Fi config, entering config mode");
    }

    return wifi_manager_enter_config_mode();
}

esp_err_t wifi_manager_verify_and_save(const char *ssid,
                                        const char *password,
                                        wifi_manager_connect_result_t *result)
{
    if (result == NULL) {
        ESP_LOGE(TAG, "verify result pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    snprintf(result->ip, sizeof(result->ip), "0.0.0.0");

    if (xSemaphoreTake(s_connect_lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "another Wi-Fi validation is running");
        result->err = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }

    wifi_manager_status_t before;
    wifi_manager_get_status(&before);

    esp_err_t err = connect_sta_blocking(ssid, password, WIFI_CONNECT_TIMEOUT_MS);
    if (err != ESP_OK) {
        result->ok = false;
        result->err = err;
        ESP_LOGW(TAG, "submitted Wi-Fi validation failed: %s", esp_err_to_name(err));
        if (before.ap_enabled) {
            state_set_mode(WIFI_MANAGER_MODE_CONFIG, true, before.ap_ssid);
        }
        xSemaphoreGive(s_connect_lock);
        return err;
    }

    err = nvs_config_save_wifi(ssid, password);
    if (err != ESP_OK) {
        result->ok = false;
        result->err = err;
        ESP_LOGE(TAG, "saving verified Wi-Fi config failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_connect_lock);
        return err;
    }

    wifi_manager_get_status(&before);
    result->ok = true;
    result->err = ESP_OK;
    snprintf(result->ip, sizeof(result->ip), "%s", before.sta_ip);
    state_set_mode(WIFI_MANAGER_MODE_STA, before.ap_enabled, before.ap_ssid);
    ESP_LOGI(TAG, "submitted Wi-Fi verified and saved, ip=%s", result->ip);

    xSemaphoreGive(s_connect_lock);
    return ESP_OK;
}

static void ap_shutdown_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));

    wifi_manager_status_t status;
    wifi_manager_get_status(&status);
    if (!status.ap_enabled) {
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "turning off SoftAP, keeping STA and HTTP server running");
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "disable SoftAP failed: %s", esp_err_to_name(err));
    } else {
        state_set_mode(WIFI_MANAGER_MODE_STA, false, NULL);
    }

    vTaskDelete(NULL);
}

void wifi_manager_schedule_ap_shutdown(void)
{
    wifi_manager_status_t status;
    wifi_manager_get_status(&status);
    if (!status.ap_enabled) {
        return;
    }

    BaseType_t ok = xTaskCreate(ap_shutdown_task, "ap_shutdown", 3072, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create ap_shutdown task");
    }
}

void wifi_manager_get_status(wifi_manager_status_t *status)
{
    if (status == NULL) {
        return;
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_state_lock);
}
