#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "nvs_config.h"

/**
 * @brief Wi-Fi 管理器当前工作模式。
 *
 * PORTAL 模式会同时开启 SoftAP，供手机/电脑连接设备并提交新的 Wi-Fi
 * 凭据；STA 模式表示设备主要作为普通 Wi-Fi 客户端接入路由器。
 */
typedef enum {
    /** 配网页面模式：SoftAP 开启，通常可通过 http://192.168.4.1 访问。 */
    WIFI_MANAGER_MODE_PORTAL = 0,
    /** 普通联网模式：设备使用已保存或刚验证成功的 Wi-Fi 凭据连接路由器。 */
    WIFI_MANAGER_MODE_STA,
} wifi_manager_mode_t;

/**
 * @brief 对外暴露的 Wi-Fi 运行状态快照。
 *
 * 该结构体由 wifi_manager_get_status() 填充。字段是一次加锁复制出来的快照，
 * 适合 HTTP 状态接口读取，不应被调用方长期缓存后当作实时状态使用。
 */
typedef struct {
    /** 当前管理器模式，决定页面显示和断线后的重连策略。 */
    wifi_manager_mode_t mode;
    /** STA 是否已经拿到 IP；只有收到 IP_EVENT_STA_GOT_IP 后才会置 true。 */
    bool sta_connected;
    /** STA IPv4 字符串，例如 "192.168.1.23"；未连接时为 "0.0.0.0"。 */
    char sta_ip[16];
    /** SoftAP 是否开启；配网成功后会延迟关闭，只保留 STA。 */
    bool ap_enabled;
    /** SoftAP SSID；SoftAP 关闭时为空字符串。 */
    char ap_ssid[33];
} wifi_manager_status_t;

/**
 * @brief 一次主动 Wi-Fi 连接尝试的结果。
 *
 * wifi_manager_try_connect() 只负责验证 SSID/密码是否能连上路由器，不保存 NVS。
 */
typedef struct {
    /** true 表示已连接成功并拿到 IP。 */
    bool ok;
    /** 连接成功后获取到的 STA IP；失败时为 "0.0.0.0"。 */
    char ip[16];
    /** 具体 esp_err_t 错误码，成功时为 ESP_OK。 */
    esp_err_t err;
} wifi_manager_connect_result_t;

/**
 * @brief 初始化 Wi-Fi 管理器内部资源。
 *
 * 会创建事件组、互斥锁、默认 STA/AP netif，配置 SoftAP 地址，并注册 Wi-Fi/IP
 * 事件处理器。该函数应在启动 STA 或进入配网模式前调用；重复调用会直接返回
 * ESP_OK。
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief 使用给定 Wi-Fi 配置启动普通 STA 模式。
 *
 * @param cfg 已从 NVS 读取并校验通过的 Wi-Fi 配置，不能为 NULL。
 *
 * 该函数只负责按调用方提供的配置连接路由器。连接失败时会返回错误，由上层决定
 * 是否回落到配网模式。
 */
esp_err_t wifi_manager_start_sta(const wifi_cfg_t *cfg);

/**
 * @brief 开启配网页面模式。
 *
 * 该函数会生成包含 MAC 后两字节的 SoftAP SSID，切换到 AP+STA 模式，并启动
 * Wi-Fi。适合在没有配置、连接失败或用户长按按键清除配置后调用。
 */
esp_err_t wifi_manager_start_portal(void);

/**
 * @brief 验证新的 Wi-Fi 凭据。
 *
 * @param cfg 待验证的 Wi-Fi 配置，SSID/密码不能为空且不能超过长度上限。
 * @param result 输出验证结果，不能为 NULL。
 *
 * 函数内部会串行化连接验证，避免多个 Web 请求同时改写 STA 配置。该函数不会写
 * NVS，调用方可在成功后自行保存配置。
 */
esp_err_t wifi_manager_try_connect(const wifi_cfg_t *cfg,
                                   wifi_manager_connect_result_t *result);

/**
 * @brief 安排异步关闭配网页面。
 *
 * 配网成功并保存 NVS 后调用。延迟关闭可以让 HTTP 响应先发回浏览器，随后切换到
 * STA-only 模式，保留已经验证成功的路由器连接。
 */
void wifi_manager_schedule_portal_stop(void);

/**
 * @brief 获取 Wi-Fi 管理器状态快照。
 *
 * @param status 输出状态指针。
 */
void wifi_manager_get_status(wifi_manager_status_t *status);
