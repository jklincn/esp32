#include "wifi_manager.h"

#include <stdlib.h>
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

/* 事件组 bit：事件回调通过它们把异步连接结果通知给阻塞等待的连接流程。 */
#define WIFI_CONNECTED_BIT BIT0

/* 单次连接验证最多等待 10 秒，期间断线事件会触发有限重试。 */
#define WIFI_CONNECT_TIMEOUT_MS 10000
#define WIFI_CONNECT_MAX_RETRY 5

/* 一次扫描从 Wi-Fi driver 取回的最大 BSS 记录数，随后会按 SSID 去重。 */
#define WIFI_SCAN_RECORD_LIMIT 32

/* 配网 SoftAP 参数。authmode 使用 WIFI_AUTH_OPEN，因此热点不需要密码。 */
#define WIFI_AP_CHANNEL 1
#define WIFI_AP_MAX_CONN 4

static const char *TAG = "[wifi_manager]";

typedef enum {
    WIFI_RUN_PORTAL = 0,
    WIFI_RUN_CONNECTING,
    WIFI_RUN_STA,
} wifi_run_state_t;

/*
 * s_wifi_event_group: STA 连接成功/失败事件同步。
 * s_connect_lock: 串行化 Wi-Fi 验证流程，避免多个 HTTP 请求同时改写 STA 配置。
 * s_state_lock: 保护 s_status，确保状态接口读到一致快照。
 * s_scan_lock: 保护 Wi-Fi 扫描缓存和后台扫描运行标记。
 */
static EventGroupHandle_t s_wifi_event_group;
static SemaphoreHandle_t s_connect_lock;
static SemaphoreHandle_t s_state_lock;
static SemaphoreHandle_t s_scan_lock;

/* 默认 STA/AP netif；AP netif 需要句柄来配置固定 192.168.4.1 地址。 */
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

/* Wi-Fi driver 是否已启动；用于避免重复 esp_wifi_start()。 */
static bool s_wifi_started;

/* 事件回调依据运行状态决定断线后是重试、自动重连还是保持配网模式。 */
static wifi_run_state_t s_run_state = WIFI_RUN_PORTAL;
static int s_retry_count;
static bool s_skip_disconnect_once;

/* 对外状态缓存。写入通过 state_set_*，读取通过 wifi_manager_get_status()。 */
static wifi_manager_status_t s_status = {
    .mode = WIFI_MANAGER_MODE_PORTAL,
    .sta_connected = false,
    .sta_ip = "0.0.0.0",
    .ap_enabled = false,
    .ap_ssid = "",
};

static wifi_scan_snapshot_t s_scan_cache = {
    .valid = false,
    .scanning = false,
    .last_error = ESP_OK,
};
static TickType_t s_scan_cache_updated_tick;
static bool s_scan_task_running;

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

/*
 * 更新管理器模式和 SoftAP 状态。
 * ap_ssid 为 NULL 且 ap_enabled=false 时清空旧 SSID，避免 SoftAP
 * 关闭后仍显示旧名称。
 */
static void state_set_mode(wifi_manager_mode_t mode, bool ap_enabled,
                           const char *ap_ssid) {
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

static void retry_connection(void) {
    if (s_retry_count >= WIFI_CONNECT_MAX_RETRY) {
        ESP_LOGW(TAG, "STA connection retry limit reached");
        return;
    }

    s_retry_count++;
    ESP_LOGI(TAG, "retrying STA connection, attempt=%d", s_retry_count);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect retry failed: %s",
                 esp_err_to_name(err));
    }
}

static void reconnect_sta(void) {
    ESP_LOGI(TAG, "reconnecting STA in normal mode");
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "normal reconnect failed: %s", esp_err_to_name(err));
    }
}

static void wifi_sta_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;

    if (event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started");
        return;
    }

    wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)
        event_data;
    ESP_LOGW(TAG, "STA disconnected, reason=%d", event ? event->reason : -1);
    state_set_sta_disconnected();
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    if (s_skip_disconnect_once) {
        s_skip_disconnect_once = false;
        return;
    }

    if (s_run_state == WIFI_RUN_CONNECTING) {
        retry_connection();
    } else if (s_run_state == WIFI_RUN_STA) {
        reconnect_sta();
    }
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
        state_set_mode(WIFI_MANAGER_MODE_STA, false, NULL);
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;
    (void)event_id;

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    char ip[16] = "0.0.0.0";
    esp_ip4addr_ntoa(&event->ip_info.ip, ip, sizeof(ip));
    ESP_LOGI(TAG, "STA got IP: %s", ip);
    s_retry_count = 0;
    s_skip_disconnect_once = false;
    state_set_sta_connected(ip);
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
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

static bool is_valid_sta_credentials(const char *ssid, const char *password) {
    return ssid != NULL && password != NULL && ssid[0] != '\0' &&
           strlen(ssid) <= WIFI_CFG_MAX_SSID_LEN &&
           strlen(password) <= WIFI_CFG_MAX_PASSWORD_LEN;
}

/* 将 SSID/密码填入 ESP-IDF 的 wifi_config_t，并设置认证兼容参数。 */
static esp_err_t build_sta_config(wifi_config_t *wifi_config, const char *ssid,
                                  const char *password) {
    if (!is_valid_sta_credentials(ssid, password)) {
        ESP_LOGE(TAG, "invalid STA credentials");
        return ESP_ERR_INVALID_ARG;
    }

    memset(wifi_config, 0, sizeof(*wifi_config));
    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);
    memcpy(wifi_config->sta.ssid, ssid, ssid_len);
    memcpy(wifi_config->sta.password, password, password_len);
    wifi_config->sta.threshold.authmode =
        password_len == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_config->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    return ESP_OK;
}

/* 确保 Wi-Fi driver 已启动。多个路径会调用该函数，因此需要幂等保护。 */
static esp_err_t ensure_wifi_started(void) {
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

/*
 * 使用给定 STA 配置发起连接，并阻塞等待成功、失败或超时。
 * 该函数只负责驱动连接流程，不负责构造配置或保存配置。
 */
static esp_err_t connect_sta_blocking(wifi_config_t *wifi_config,
                                      int timeout_ms,
                                      wifi_run_state_t success_state,
                                      wifi_run_state_t failure_state) {
    /* 清理上一次连接结果，避免旧 bit 让本次等待立即返回。 */
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    state_set_sta_disconnected();
    s_retry_count = 0;
    s_run_state = failure_state;

    /*
     * 主动断开旧连接后再设置新配置。未启动或本来未连接都不是致命错误，
     * 因为后面会重新 set_config/start/connect。
     */
    s_skip_disconnect_once = false;

    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_OK) {
        s_skip_disconnect_once = true;
    }
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED &&
        err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "disconnect before connect failed: %s",
                 esp_err_to_name(err));
    }

    err = esp_wifi_set_config(WIFI_IF_STA, wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set STA config failed: %s", esp_err_to_name(err));
        s_skip_disconnect_once = false;
        s_run_state = failure_state;
        return err;
    }

    err = ensure_wifi_started();
    if (err != ESP_OK) {
        s_skip_disconnect_once = false;
        s_run_state = failure_state;
        return err;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    s_retry_count = 0;
    s_run_state = WIFI_RUN_CONNECTING;

    ESP_LOGI(TAG, "connecting STA to ssid=%s", wifi_config->sta.ssid);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        s_skip_disconnect_once = false;
        s_run_state = failure_state;
        return err;
    }

    /* 等待 IP 事件设置成功 bit；超时则认为本次验证失败。 */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "STA connected");
        s_run_state = success_state;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "STA connection timed out");
    s_skip_disconnect_once = false;
    s_run_state = failure_state;
    esp_wifi_disconnect();
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_manager_start_sta(const wifi_cfg_t *cfg) {
    ESP_LOGI(TAG, "starting STA mode");

    wifi_config_t wifi_config;
    esp_err_t err = build_sta_config(&wifi_config, cfg->ssid, cfg->password);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set WIFI_MODE_STA failed: %s", esp_err_to_name(err));
        return err;
    }

    state_set_mode(WIFI_MANAGER_MODE_STA, false, NULL);

    err = connect_sta_blocking(&wifi_config, WIFI_CONNECT_TIMEOUT_MS,
                               WIFI_RUN_STA, WIFI_RUN_PORTAL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saved Wi-Fi connection failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t wifi_manager_init(void) {
    /* 已初始化时直接返回，避免重复创建 netif 或重复注册事件处理器。 */
    if (s_wifi_event_group != NULL) {
        return ESP_OK;
    }

    s_wifi_event_group = xEventGroupCreate();
    s_connect_lock = xSemaphoreCreateMutex();
    s_state_lock = xSemaphoreCreateMutex();
    s_scan_lock = xSemaphoreCreateMutex();
    if (s_wifi_event_group == NULL || s_connect_lock == NULL ||
        s_state_lock == NULL || s_scan_lock == NULL) {
        ESP_LOGE(TAG, "failed to create synchronization primitives");
        return ESP_ERR_NO_MEM;
    }

    /*
     * 同时创建 STA 和 AP netif，后续可在 WIFI_MODE_STA 与 WIFI_MODE_APSTA
     * 间切换。 AP netif 即使暂时不启用，也需要先配置固定 IP。
     */
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

    /*
     * 使用 RAM 存储 Wi-Fi 配置，避免 esp_wifi_set_config() 自动写入系统 Wi-Fi
     * NVS。 本项目只通过 app_nvs.c 保存经过验证的配置。
     */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set Wi-Fi storage RAM failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * 关闭省电可以降低 HTTP 访问在部分路由器下的延迟和丢包概率。
     * 失败不影响基础联网，所以这里只记录警告。
     */
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "disable Wi-Fi power save failed: %s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi power save disabled for HTTP reliability");
    }

    /* STA、AP 和 IP 事件分开注册，启动路线和配网页面路线互不掺杂。 */
    err = esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_START, wifi_sta_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register STA_START handler failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_sta_event_handler, NULL,
        NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register STA_DISCONNECTED handler failed: %s",
                 esp_err_to_name(err));
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

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              ip_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register STA_GOT_IP handler failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "wifi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_manager_start_portal(void) {
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
    s_run_state = WIFI_RUN_PORTAL;

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

    err = ensure_wifi_started();
    if (err != ESP_OK) {
        return err;
    }

    state_set_mode(WIFI_MANAGER_MODE_PORTAL, true, ap_ssid);
    ESP_LOGI(TAG, "portal mode ready at http://192.168.4.1");
    err = wifi_manager_request_scan();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start initial Wi-Fi scan failed: %s",
                 esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t wifi_manager_try_connect(const wifi_cfg_t *cfg,
                                   wifi_manager_connect_result_t *result) {
    if (result == NULL) {
        ESP_LOGE(TAG, "connect result pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    snprintf(result->ip, sizeof(result->ip), "0.0.0.0");

    /* 同一时间只允许一个配网验证请求运行，避免并发请求互相覆盖 STA 配置。 */
    if (xSemaphoreTake(s_connect_lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "another Wi-Fi validation is running");
        result->err = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }

    wifi_manager_status_t before;
    wifi_manager_get_status(&before);

    wifi_run_state_t return_state = before.mode == WIFI_MANAGER_MODE_STA
                                        ? WIFI_RUN_STA
                                        : WIFI_RUN_PORTAL;

    /* 验证失败时恢复调用前的运行路线，让配网页面继续可用。 */
    wifi_config_t wifi_config;
    esp_err_t err = build_sta_config(&wifi_config, cfg->ssid, cfg->password);
    if (err != ESP_OK) {
        result->ok = false;
        result->err = err;
        xSemaphoreGive(s_connect_lock);
        return err;
    }

    err = connect_sta_blocking(&wifi_config, WIFI_CONNECT_TIMEOUT_MS,
                               return_state, return_state);
    if (err != ESP_OK) {
        result->ok = false;
        result->err = err;
        ESP_LOGW(TAG, "submitted Wi-Fi connection failed: %s",
                 esp_err_to_name(err));
        if (before.ap_enabled) {
            state_set_mode(WIFI_MANAGER_MODE_PORTAL, true, before.ap_ssid);
        } else {
            state_set_mode(WIFI_MANAGER_MODE_STA, false, NULL);
        }
        xSemaphoreGive(s_connect_lock);
        return err;
    }

    wifi_manager_get_status(&before);
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

    esp_err_t err = ensure_wifi_started();
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

    wifi_ap_record_t *records =
        calloc(WIFI_SCAN_RECORD_LIMIT, sizeof(wifi_ap_record_t));
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
        size_t ssid_len =
            strnlen((const char *)records[i].ssid, WIFI_CFG_MAX_SSID_LEN);
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
        ESP_LOGW(TAG, "background Wi-Fi scan failed: %s",
                 esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

esp_err_t wifi_manager_request_scan(void) {
    if (s_scan_lock == NULL) {
        ESP_LOGE(TAG, "scan requested before wifi manager init");
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

void wifi_manager_get_scan_snapshot(wifi_scan_snapshot_t *snapshot) {
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
        snapshot->age_ms =
            age_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)age_ms;
    }
    xSemaphoreGive(s_scan_lock);
}

void wifi_manager_get_status(wifi_manager_status_t *status) {
    /* 用互斥锁保护结构体整体复制，避免 Web API 读到半更新状态。 */
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_state_lock);
}
