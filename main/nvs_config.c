#include "nvs_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "nvs_config";

bool nvs_config_is_wifi_valid(const wifi_cfg_t *cfg)
{
    if (cfg == NULL) {
        return false;
    }

    return cfg->initialized &&
           cfg->ssid[0] != '\0' &&
           cfg->password[0] != '\0' &&
           strnlen(cfg->ssid, sizeof(cfg->ssid)) <= WIFI_CFG_MAX_SSID_LEN &&
           strnlen(cfg->password, sizeof(cfg->password)) <= WIFI_CFG_MAX_PASSWORD_LEN;
}

esp_err_t nvs_config_load_wifi(wifi_cfg_t *cfg)
{
    if (cfg == NULL) {
        ESP_LOGE(TAG, "load_wifi called with NULL cfg");
        return ESP_ERR_INVALID_ARG;
    }

    memset(cfg, 0, sizeof(*cfg));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CFG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open namespace %s failed: %s", WIFI_CFG_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    uint8_t initialized = 0;
    err = nvs_get_u8(handle, WIFI_CFG_KEY_INITIALIZED, &initialized);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read initialized failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }
    cfg->initialized = initialized == 1;

    size_t ssid_len = sizeof(cfg->ssid);
    err = nvs_get_str(handle, WIFI_CFG_KEY_SSID, cfg->ssid, &ssid_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read ssid failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    size_t password_len = sizeof(cfg->password);
    err = nvs_get_str(handle, WIFI_CFG_KEY_PASSWORD, cfg->password, &password_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read password failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    nvs_close(handle);

    if (!nvs_config_is_wifi_valid(cfg)) {
        ESP_LOGW(TAG, "wifi config exists but is invalid");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "wifi config loaded, ssid=%s", cfg->ssid);
    return ESP_OK;
}

esp_err_t nvs_config_save_wifi(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL ||
        ssid[0] == '\0' || password[0] == '\0' ||
        strlen(ssid) > WIFI_CFG_MAX_SSID_LEN ||
        strlen(password) > WIFI_CFG_MAX_PASSWORD_LEN) {
        ESP_LOGE(TAG, "invalid wifi config input");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CFG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open namespace %s failed: %s", WIFI_CFG_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, WIFI_CFG_KEY_SSID, ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write ssid failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_set_str(handle, WIFI_CFG_KEY_PASSWORD, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write password failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_set_u8(handle, WIFI_CFG_KEY_INITIALIZED, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write initialized failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit wifi config failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "wifi config saved, ssid=%s", ssid);
    return ESP_OK;
}

esp_err_t nvs_config_clear_wifi(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CFG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "namespace %s not found, nothing to clear", WIFI_CFG_NAMESPACE);
            return ESP_OK;
        }
        ESP_LOGE(TAG, "open namespace %s failed: %s", WIFI_CFG_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    err = nvs_erase_all(handle);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "erase wifi config failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit erase failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "wifi config cleared");
    return ESP_OK;
}
