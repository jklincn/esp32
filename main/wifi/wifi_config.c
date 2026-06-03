#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"  // IWYU pragma: keep
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi/wifi.h"
#include "wifi/wifi_internal.h"

/* 一次扫描从 Wi-Fi driver 取回的最大 BSS 记录数，随后会按 SSID 去重。 */
#define WIFI_SCAN_RECORD_LIMIT 32

/* 配网 SoftAP 参数。authmode 使用 WIFI_AUTH_OPEN，因此热点不需要密码。 */
#define WIFI_AP_CHANNEL 1
#define WIFI_AP_MAX_CONN 4

static const char *TAG = "[wifi]";

static SemaphoreHandle_t s_connect_lock;
static SemaphoreHandle_t s_scan_lock;
static esp_netif_t *s_ap_netif;
static bool s_validating_connection;
static bool s_skip_disconnect_once;
static int s_retry_count;

static wifi_scan_snapshot_t s_scan_cache = {
    .valid = false,
    .scanning = false,
    .last_error = ESP_OK,
};
static TickType_t s_scan_cache_updated_tick;
static bool s_scan_task_running;

static void retry_connection(void) {
    if (s_retry_count >= WIFI_CONNECT_MAX_RETRY) {
        ESP_LOGW(TAG, "config STA validation retry limit reached");
        return;
    }

    s_retry_count++;
    ESP_LOGI(TAG, "retrying config STA validation, attempt=%d", s_retry_count);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config STA retry failed: %s", esp_err_to_name(err));
    }
}

static void config_sta_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;

    if (event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "config STA started");
        return;
    }

    wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)
        event_data;
    ESP_LOGW(TAG, "config STA disconnected, reason=%d",
             event ? event->reason : -1);
    wifi_core_handle_sta_disconnected();

    if (s_skip_disconnect_once) {
        s_skip_disconnect_once = false;
        return;
    }

    if (s_validating_connection) {
        retry_connection();
    }
}

static void config_ip_event_handler(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;
    (void)event_id;

    s_retry_count = 0;
    s_skip_disconnect_once = false;
    wifi_core_handle_sta_got_ip(event_data);
}

static void wifi_ap_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "SoftAP started");
    } else if (event_id == WIFI_EVENT_AP_STOP) {
        ESP_LOGI(TAG, "SoftAP stopped");
        wifi_core_set_mode(WIFI_MODE_STA, NULL);
    }
}

/*
 * 固定 SoftAP 地址为 192.168.4.1，并重启 DHCP server。
 * 用户连接设备热点后，就能用固定地址打开配网页面。
 */
static esp_err_t configure_ap_ip(void) {
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

static esp_err_t init_config_wifi(void) {
    esp_err_t err = wifi_core_init_common_resources();
    if (err != ESP_OK) {
        return err;
    }

    s_connect_lock = xSemaphoreCreateMutex();
    s_scan_lock = xSemaphoreCreateMutex();
    if (s_connect_lock == NULL || s_scan_lock == NULL) {
        ESP_LOGE(TAG, "failed to create config synchronization primitives");
        return ESP_ERR_NO_MEM;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        ESP_LOGE(TAG, "failed to create default SoftAP netif");
        return ESP_ERR_NO_MEM;
    }

    err = configure_ap_ip();
    if (err != ESP_OK) {
        return err;
    }

    err = wifi_core_init_driver();
    if (err != ESP_OK) {
        return err;
    }

    err = wifi_event_handler_register(config_sta_event_handler,
                                      config_ip_event_handler);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_AP_START, wifi_ap_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register AP_START handler failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_AP_STOP, wifi_ap_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register AP_STOP handler failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Wi-Fi initialized in config mode");
    return ESP_OK;
}

static esp_err_t start_config_mode(void) {
    /* SoftAP 名称带 MAC 后两字节，方便附近有多台设备时区分。 */
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
    ap_config.ap.channel = WIFI_AP_CHANNEL;
    ap_config.ap.max_connection = WIFI_AP_MAX_CONN;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.pmf_cfg.required = false;

    ESP_LOGI(TAG, "starting portal mode, SoftAP SSID=%s", ap_ssid);

    /* APSTA 模式允许设备一边开热点给用户配网，一边尝试连接用户提交的路由器。 */
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

    err = wifi_core_ensure_started();
    if (err != ESP_OK) {
        return err;
    }

    wifi_core_set_mode(WIFI_MODE_APSTA, ap_ssid);
    ESP_LOGI(TAG, "portal mode ready at http://192.168.4.1");
    err = wifi_request_scan();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start initial Wi-Fi scan failed: %s",
                 esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t wifi_start_config(void) {
    esp_err_t err = init_config_wifi();
    if (err != ESP_OK) {
        return err;
    }

    return start_config_mode();
}

static esp_err_t validate_sta_connection(wifi_config_t *wifi_config) {
    wifi_core_reset_sta_connection_result();
    s_validating_connection = true;
    s_retry_count = 0;
    s_skip_disconnect_once = wifi_core_disconnect_sta();

    esp_err_t err = wifi_core_start_sta_connect(wifi_config);
    if (err != ESP_OK) {
        s_validating_connection = false;
        s_skip_disconnect_once = false;
        return err;
    }

    err = wifi_core_wait_sta_connected(WIFI_CONNECT_TIMEOUT_MS);
    s_validating_connection = false;
    if (err != ESP_OK) {
        s_skip_disconnect_once = wifi_core_disconnect_sta();
        return err;
    }

    return ESP_OK;
}

esp_err_t wifi_try_connect(const wifi_cfg_t *cfg,
                           wifi_connect_result_t *result) {
    if (result == NULL) {
        ESP_LOGE(TAG, "connect result pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    snprintf(result->ip, sizeof(result->ip), "0.0.0.0");

    if (s_connect_lock == NULL) {
        ESP_LOGE(TAG, "Wi-Fi validation requested outside config mode");
        result->err = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }

    /* 同一时间只允许一个配网验证请求运行，避免并发请求互相覆盖 STA 配置。 */
    if (xSemaphoreTake(s_connect_lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "another Wi-Fi validation is running");
        result->err = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }

    wifi_status_t before;
    wifi_get_status(&before);

    /* 验证失败时恢复调用前的运行路线，让配网页面继续可用。 */
    wifi_config_t wifi_config;
    esp_err_t err = wifi_core_build_sta_config(&wifi_config, cfg->ssid,
                                               cfg->password);
    if (err != ESP_OK) {
        result->ok = false;
        result->err = err;
        xSemaphoreGive(s_connect_lock);
        return err;
    }

    err = validate_sta_connection(&wifi_config);
    if (err != ESP_OK) {
        result->ok = false;
        result->err = err;
        ESP_LOGW(TAG, "submitted Wi-Fi connection failed: %s",
                 esp_err_to_name(err));
        if (before.mode == WIFI_MODE_APSTA) {
            wifi_core_set_mode(WIFI_MODE_APSTA, before.ap_ssid);
        } else {
            wifi_core_set_mode(WIFI_MODE_STA, NULL);
        }
        xSemaphoreGive(s_connect_lock);
        return err;
    }

    wifi_get_status(&before);
    result->ok = true;
    result->err = ESP_OK;
    snprintf(result->ip, sizeof(result->ip), "%s", before.sta_ip);
    ESP_LOGI(TAG, "submitted Wi-Fi verified, ip=%s", result->ip);

    xSemaphoreGive(s_connect_lock);
    return ESP_OK;
}

static bool authmode_requires_password(wifi_auth_mode_t authmode) {
    return authmode != WIFI_AUTH_OPEN && authmode != WIFI_AUTH_OWE;
}

static bool scan_result_has_ssid(const wifi_scan_ap_t *aps, uint16_t count,
                                 const char *ssid) {
    for (uint16_t i = 0; i < count; i++) {
        if (strcmp(aps[i].ssid, ssid) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t scan_nearby_aps(wifi_scan_ap_t *aps, uint16_t max_aps,
                                 uint16_t *ap_count) {
    if (aps == NULL || max_aps == 0 || ap_count == NULL) {
        ESP_LOGE(TAG, "invalid scan output arguments");
        return ESP_ERR_INVALID_ARG;
    }
    *ap_count = 0;

    /*
     * 扫描和连接都会占用 STA 控制流程。共用连接锁可以避免用户一边点连接、
     * 页面一边刷新列表时互相打断。
     */
    if (xSemaphoreTake(s_connect_lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "scan skipped because Wi-Fi validation is running");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = wifi_core_ensure_started();
    if (err != ESP_OK) {
        xSemaphoreGive(s_connect_lock);
        return err;
    }

    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 0,
        .scan_time.active.max = 120,
        .home_chan_dwell_time = 30,
    };

    ESP_LOGI(TAG, "scanning nearby Wi-Fi APs");
    err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_connect_lock);
        if (err == ESP_ERR_WIFI_STATE) {
            return ESP_ERR_INVALID_STATE;
        }
        return err;
    }

    wifi_ap_record_t *records = calloc(WIFI_SCAN_RECORD_LIMIT,
                                       sizeof(wifi_ap_record_t));
    if (records == NULL) {
        ESP_LOGE(TAG, "alloc scan record buffer failed");
        esp_wifi_clear_ap_list();
        xSemaphoreGive(s_connect_lock);
        return ESP_ERR_NO_MEM;
    }

    uint16_t record_count = WIFI_SCAN_RECORD_LIMIT;
    err = esp_wifi_scan_get_ap_records(&record_count, records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get Wi-Fi scan records failed: %s",
                 esp_err_to_name(err));
        free(records);
        xSemaphoreGive(s_connect_lock);
        return err;
    }

    for (uint16_t i = 0; i < record_count && *ap_count < max_aps; i++) {
        char ssid[WIFI_CFG_MAX_SSID_LEN + 1] = {0};
        size_t ssid_len = strnlen((const char *)records[i].ssid,
                                  WIFI_CFG_MAX_SSID_LEN);
        if (ssid_len == 0) {
            continue;
        }

        memcpy(ssid, records[i].ssid, ssid_len);
        if (scan_result_has_ssid(aps, *ap_count, ssid)) {
            continue;
        }

        wifi_scan_ap_t *ap = &aps[*ap_count];
        snprintf(ap->ssid, sizeof(ap->ssid), "%s", ssid);
        ap->rssi = records[i].rssi;
        ap->channel = records[i].primary;
        ap->password_required = authmode_requires_password(records[i].authmode);
        (*ap_count)++;
    }

    free(records);
    xSemaphoreGive(s_connect_lock);

    ESP_LOGI(TAG, "Wi-Fi scan returned %u APs", (unsigned)*ap_count);
    return ESP_OK;
}

static void wifi_scan_task(void *arg) {
    (void)arg;

    wifi_scan_ap_t aps[WIFI_SCAN_MAX_APS];
    uint16_t ap_count = 0;
    esp_err_t err = scan_nearby_aps(aps, WIFI_SCAN_MAX_APS, &ap_count);

    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    if (err == ESP_OK) {
        memset(&s_scan_cache, 0, sizeof(s_scan_cache));
        memcpy(s_scan_cache.aps, aps, ap_count * sizeof(aps[0]));
        s_scan_cache.ap_count = ap_count;
        s_scan_cache.valid = true;
        s_scan_cache_updated_tick = xTaskGetTickCount();
    }
    s_scan_cache.scanning = false;
    s_scan_cache.last_error = err;
    s_scan_task_running = false;
    xSemaphoreGive(s_scan_lock);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "background Wi-Fi scan failed: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

esp_err_t wifi_request_scan(void) {
    if (s_scan_lock == NULL) {
        ESP_LOGE(TAG, "scan requested before Wi-Fi config mode init");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    if (s_scan_task_running) {
        s_scan_cache.scanning = true;
        xSemaphoreGive(s_scan_lock);
        return ESP_OK;
    }
    s_scan_cache.scanning = true;
    s_scan_task_running = true;
    xSemaphoreGive(s_scan_lock);

    BaseType_t ok = xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 3,
                                NULL);
    if (ok == pdPASS) {
        return ESP_OK;
    }

    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    s_scan_cache.scanning = false;
    s_scan_cache.last_error = ESP_ERR_NO_MEM;
    s_scan_task_running = false;
    xSemaphoreGive(s_scan_lock);
    ESP_LOGE(TAG, "failed to create Wi-Fi scan task");
    return ESP_ERR_NO_MEM;
}

void wifi_get_scan_snapshot(wifi_scan_snapshot_t *snapshot) {
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    if (s_scan_lock == NULL) {
        snapshot->last_error = ESP_ERR_INVALID_STATE;
        return;
    }

    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    *snapshot = s_scan_cache;
    if (snapshot->valid) {
        TickType_t age_ticks = xTaskGetTickCount() - s_scan_cache_updated_tick;
        uint64_t age_ms = (uint64_t)age_ticks * portTICK_PERIOD_MS;
        snapshot->age_ms = age_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)age_ms;
    }
    xSemaphoreGive(s_scan_lock);
}
