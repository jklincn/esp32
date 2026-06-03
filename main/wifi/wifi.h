#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types_generic.h"
#include "storage/storage.h"

#define WIFI_SCAN_MAX_APS 20

/**
 * @brief 对外暴露的 Wi-Fi 运行状态快照。
 *
 * 该结构体由 wifi_get_status() 填充。字段是一次加锁复制出来的快照，
 * 适合 HTTP 状态接口读取，不应被调用方长期缓存后当作实时状态使用。
 */
typedef struct {
    /** 当前 Wi-Fi 模式，决定页面显示和断线后的重连策略。 */
    wifi_mode_t mode;
    /** STA 是否已经拿到 IP；只有收到 IP_EVENT_STA_GOT_IP 后才会置 true。 */
    bool sta_connected;
    /** STA IPv4 字符串，例如 "192.168.1.23"；未连接时为 "0.0.0.0"。 */
    char sta_ip[16];
    /** SoftAP SSID；SoftAP 关闭时为空字符串。 */
    char ap_ssid[33];
} wifi_status_t;

/**
 * @brief 一次主动 Wi-Fi 连接尝试的结果。
 *
 * wifi_try_connect() 只负责验证 SSID/密码是否能连上路由器，不保存 NVS。
 */
typedef struct {
    /** true 表示已连接成功并拿到 IP。 */
    bool ok;
    /** 连接成功后获取到的 STA IP；失败时为 "0.0.0.0"。 */
    char ip[16];
    /** 具体 esp_err_t 错误码，成功时为 ESP_OK。 */
    esp_err_t err;
} wifi_connect_result_t;

/**
 * @brief 扫描到的 Wi-Fi 热点摘要。
 *
 * 只暴露配网页面需要展示和提交的信息，避免 HTTP 层直接依赖 ESP-IDF 的
 * wifi_ap_record_t。
 */
typedef struct {
    /** SSID，已保证以 '\0' 结尾；隐藏网络不会出现在扫描结果中。 */
    char ssid[WIFI_CFG_MAX_SSID_LEN + 1];
    /** RSSI 信号强度，数值越接近 0 表示信号越强。 */
    int8_t rssi;
    /** 主信道。 */
    uint8_t channel;
    /** true 表示连接该网络需要密码。 */
    bool password_required;
} wifi_scan_ap_t;

/**
 * @brief Wi-Fi 扫描缓存快照。
 *
 * 配网页面读取的是这个 RAM 缓存，刷新按钮只触发后台扫描，不会让 HTTP
 * 请求长时间阻塞。
 */
typedef struct {
    /** 最近一次成功扫描到的热点列表。 */
    wifi_scan_ap_t aps[WIFI_SCAN_MAX_APS];
    /** aps 中有效条目数量。 */
    uint16_t ap_count;
    /** true 表示已经有一次可展示的扫描结果，即使 ap_count 为 0。 */
    bool valid;
    /** true 表示后台扫描任务正在运行。 */
    bool scanning;
    /** 缓存年龄，单位毫秒；valid=false 时为 0。 */
    uint32_t age_ms;
    /** 最近一次后台扫描的错误码，成功时为 ESP_OK。 */
    esp_err_t last_error;
} wifi_scan_snapshot_t;

/**
 * @brief 使用给定 Wi-Fi 配置启动普通 STA 模式。
 *
 * @param cfg 已从 NVS 读取并校验通过的 Wi-Fi 配置，不能为 NULL。
 *
 * 该函数会初始化普通 STA 模式需要的 Wi-Fi
 * 资源，并按调用方提供的配置连接路由器。
 * 连接失败时会返回错误，由上层决定是否进入错误状态。
 */
esp_err_t wifi_start_normal(const wifi_cfg_t *cfg);

/**
 * @brief 开启配网页面模式。
 *
 * 该函数会初始化配置模式需要的 Wi-Fi 资源，生成包含 MAC 后两字节的 SoftAP
 * SSID， 切换到 AP+STA 模式，并启动 Wi-Fi。
 */
esp_err_t wifi_start_config(void);

/**
 * @brief 验证新的 Wi-Fi 凭据。
 *
 * @param cfg 待验证的 Wi-Fi 配置，SSID/密码不能为空且不能超过长度上限。
 * @param result 输出验证结果，不能为 NULL。
 *
 * 函数内部会串行化连接验证，避免多个 Web 请求同时改写 STA 配置。该函数不会写
 * NVS，调用方可在成功后自行保存配置。
 */
esp_err_t wifi_try_connect(const wifi_cfg_t *cfg,
                           wifi_connect_result_t *result);

/**
 * @brief 请求后台刷新 Wi-Fi 扫描缓存。
 *
 * 如果扫描已经在运行，会直接返回 ESP_OK；调用方随后可用
 * wifi_get_scan_snapshot() 查看 scanning 状态。
 */
esp_err_t wifi_request_scan(void);

/**
 * @brief 获取当前 Wi-Fi 扫描缓存快照。
 *
 * @param snapshot 输出快照指针。
 */
void wifi_get_scan_snapshot(wifi_scan_snapshot_t *snapshot);

/**
 * @brief 获取 Wi-Fi 状态快照。
 *
 * @param status 输出状态指针。
 */
void wifi_get_status(wifi_status_t *status);
