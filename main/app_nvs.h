#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define WIFI_CFG_NAMESPACE "wifi_cfg"
#define WIFI_CFG_KEY_SSID "ssid"
#define WIFI_CFG_KEY_PASSWORD "password"
#define WIFI_CFG_KEY_INITIALIZED "initialized"

#define WIFI_CFG_MAX_SSID_LEN 32
#define WIFI_CFG_MAX_PASSWORD_LEN 64

// 保存在 NVS 中的 Wi-Fi 配置。
// initialized 用于区分“namespace 存在但尚未完成写入”和“配置已经完整保存”。
// app_nvs_load_wifi() 返回 ESP_OK 时，表示配置已经通过完整校验。
typedef struct {
    char ssid[WIFI_CFG_MAX_SSID_LEN + 1];
    char password[WIFI_CFG_MAX_PASSWORD_LEN + 1];
    bool initialized;
} wifi_cfg_t;

// 初始化 NVS flash
esp_err_t app_nvs_init(void);

// 判断读取 Wi-Fi 配置失败是否代表“没有可用配置”，调用方可据此进入配网模式
bool app_nvs_is_wifi_config_unavailable(esp_err_t err);

// 从 NVS 读取 Wi-Fi 配置
esp_err_t app_nvs_load_wifi(wifi_cfg_t *cfg);

// 保存 Wi-Fi 配置到 NVS
esp_err_t app_nvs_save_wifi(const char *ssid, const char *password);

// 清除已保存的 Wi-Fi 配置
esp_err_t app_nvs_clear_wifi(void);
