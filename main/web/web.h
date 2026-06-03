#pragma once

#include "esp_err.h"

typedef enum {
    WEB_SERVER_MODE_NORMAL,
    WEB_SERVER_MODE_CONFIG,
} web_server_mode_t;

/**
 * @brief 启动 HTTP 页面和 API 服务。
 *
 * NORMAL 模式注册：
 * - GET /             返回正常运行状态页面
 * - GET /api/status   返回当前 Wi-Fi 状态 JSON
 *
 * CONFIG 模式额外注册：
 * - GET  /api/wifi_scan   返回 Wi-Fi 扫描缓存，refresh=1 时触发后台刷新
 * - POST /api/wifi_config 验证并保存用户提交的 SSID/密码
 *
 * 重复调用会直接返回 ESP_OK，不会启动第二个 HTTP server。
 */
esp_err_t web_server_start(web_server_mode_t mode);
