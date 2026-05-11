#include "button_manager.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_config.h"

#define BOOT_BUTTON_GPIO GPIO_NUM_9
#define BUTTON_POLL_MS 50
#define BUTTON_RESTART_MS 1000
#define BUTTON_CLEAR_WIFI_MS 5000

static const char *TAG = "button_manager";
static TaskHandle_t s_button_task;

static bool button_is_pressed(void)
{
    return gpio_get_level(BOOT_BUTTON_GPIO) == 0;
}

static void button_task(void *arg)
{
    (void)arg;
    bool was_pressed = false;
    TickType_t pressed_at = 0;
    bool long_press_hint_logged = false;

    while (true) {
        bool pressed = button_is_pressed();
        TickType_t now = xTaskGetTickCount();

        if (pressed && !was_pressed) {
            pressed_at = now;
            long_press_hint_logged = false;
            ESP_LOGI(TAG, "BOOT button pressed");
        } else if (pressed && was_pressed && !long_press_hint_logged) {
            uint32_t held_ms = pdTICKS_TO_MS(now - pressed_at);
            if (held_ms >= BUTTON_CLEAR_WIFI_MS) {
                ESP_LOGW(TAG, "BOOT button held for 5s, release to clear Wi-Fi config and restart");
                long_press_hint_logged = true;
            }
        } else if (!pressed && was_pressed) {
            uint32_t held_ms = pdTICKS_TO_MS(now - pressed_at);
            ESP_LOGI(TAG, "BOOT button released, held=%" PRIu32 " ms", held_ms);

            if (held_ms >= BUTTON_CLEAR_WIFI_MS) {
                ESP_LOGW(TAG, "clearing Wi-Fi config by BOOT button long press");
                esp_err_t err = nvs_config_clear_wifi();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "clear Wi-Fi config failed: %s", esp_err_to_name(err));
                }
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            } else if (held_ms >= BUTTON_RESTART_MS) {
                ESP_LOGI(TAG, "restarting by BOOT button press");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        }

        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t button_manager_start(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configure BOOT button GPIO failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(button_task, "button_task", 3072, NULL, 5, &s_button_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create button task failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "BOOT button enabled: release after 1s to restart, release after 5s to clear Wi-Fi and restart");
    return ESP_OK;
}
