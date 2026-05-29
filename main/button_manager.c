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

static bool button_is_pressed(void) {
    return gpio_get_level(BOOT_BUTTON_GPIO) == 0;
}

static void button_task(void *arg) {
    (void)arg;
    TickType_t press_start_tick = 0;

    // 用于判断“刚按下”和“刚松开”。
    bool last_pressed = false;

    /*
     * BOOT 按键接在 GPIO9 和 GND 之间，是低电平有效：
     *   - 松开：GPIO9 为高电平，button_is_pressed() 返回 false
     *   - 按下：GPIO9 为低电平，button_is_pressed() 返回 true
     *
     * GPIO9 也是启动模式选择引脚，所以重启操作放在“松开后”执行，
     * 避免按住 BOOT 时重启导致芯片进入下载模式。
     *
     * 行为：
     *   - 按住 >= 1 秒后松开：重启
     *   - 按住 >= 5 秒后松开：清除 Wi-Fi 配置并重启
     */
    while (true) {
        // 读取当前按钮状态。
        bool pressed = button_is_pressed();

        // 获取当前系统 tick。
        TickType_t now = xTaskGetTickCount();

        // 当前按下、上一轮未按下，说明刚刚按下。
        if (pressed && !last_pressed) {
            // 记录按下开始时间。
            press_start_tick = now;

            ESP_LOGI(TAG, "BOOT button pressed");
        }

        // 当前松开、上一轮按下，说明刚刚松开。
        if (!pressed && last_pressed) {
            // 计算本次按住时间，单位为毫秒。
            uint32_t held_ms = pdTICKS_TO_MS(now - press_start_tick);

            ESP_LOGI(TAG, "BOOT button released, held=%" PRIu32 " ms", held_ms);

            // 长按 5 秒：清除 Wi-Fi 配置并重启。
            if (held_ms >= BUTTON_CLEAR_WIFI_MS) {
                ESP_LOGW(TAG,
                         "clearing Wi-Fi config by BOOT button long press");

                esp_err_t err = nvs_config_clear_wifi();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "clear Wi-Fi config failed: %s",
                             esp_err_to_name(err));
                }

                // 延迟 200ms，给日志输出和系统处理留一点时间
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }

            // 长按 1 秒：普通重启。
            else if (held_ms >= BUTTON_RESTART_MS) {
                ESP_LOGI(TAG, "restarting by BOOT button press");

                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        }

        // 保存当前状态，下一轮用来判断状态变化。
        last_pressed = pressed;

        // 每隔 BUTTON_POLL_MS 毫秒轮询一次按钮。
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t button_manager_start(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configure BOOT button GPIO failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    BaseType_t ok =
        xTaskCreate(button_task, "button_task", 3072, NULL, 5, &s_button_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create button task failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "BOOT button enabled: release after 1s to restart, release after "
             "5s to clear Wi-Fi and restart");
    return ESP_OK;
}
