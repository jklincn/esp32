#pragma once

#include "esp_err.h"

/**
 * @brief 启动 HTTP 配网页面和 API 服务。
 *
 * 注册以下路由：
 * - GET  /              返回内置 HTML 配网页面
 * - GET  /api/status    返回当前 Wi-Fi 状态 JSON
 * - POST /api/wifi_config 验证并保存用户提交的 SSID/密码
 *
 * 重复调用会直接返回 ESP_OK，不会启动第二个 HTTP server。
 */
esp_err_t web_server_start(void);
