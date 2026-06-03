#include "led/led.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"

#define SYSTEM_LED_GPIO GPIO_NUM_8
#define SYSTEM_LED_RESOLUTION_HZ 10000000
#define SYSTEM_LED_MEM_BLOCK_SYMBOLS 64
#define SYSTEM_LED_BRIGHTNESS 16
#define SYSTEM_LED_TASK_PERIOD_MS 50
#define SYSTEM_LED_GREEN_BLINK_MS 600

static const char *TAG = "[led]";

typedef enum {
    SYSTEM_LED_EFFECT_OFF,
    SYSTEM_LED_EFFECT_GREEN,
    SYSTEM_LED_EFFECT_GREEN_BLINK,
    SYSTEM_LED_EFFECT_BLUE,
    SYSTEM_LED_EFFECT_RED,
} system_led_effect_t;

static led_strip_handle_t s_led_strip;
static SemaphoreHandle_t s_led_lock;
static TaskHandle_t s_led_task;
static system_led_effect_t s_effect = SYSTEM_LED_EFFECT_OFF;

static esp_err_t write_rgb(uint8_t red, uint8_t green, uint8_t blue) {
    esp_err_t err = led_strip_set_pixel(s_led_strip, 0, red, green, blue);
    if (err != ESP_OK) {
        return err;
    }

    return led_strip_refresh(s_led_strip);
}

static esp_err_t write_effect(system_led_effect_t effect) {
    switch (effect) {
        case SYSTEM_LED_EFFECT_OFF:
            return write_rgb(0, 0, 0);
        case SYSTEM_LED_EFFECT_GREEN:
        case SYSTEM_LED_EFFECT_GREEN_BLINK:
            return write_rgb(0, SYSTEM_LED_BRIGHTNESS, 0);
        case SYSTEM_LED_EFFECT_BLUE:
            return write_rgb(0, 0, SYSTEM_LED_BRIGHTNESS);
        case SYSTEM_LED_EFFECT_RED:
            return write_rgb(SYSTEM_LED_BRIGHTNESS, 0, 0);
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static bool is_blinking_effect(system_led_effect_t effect) {
    return effect == SYSTEM_LED_EFFECT_GREEN_BLINK;
}

static void system_led_task(void *arg) {
    (void)arg;

    system_led_effect_t active_effect = SYSTEM_LED_EFFECT_OFF;
    bool has_active_effect = false;
    bool blink_on = true;
    TickType_t last_toggle_tick = 0;

    while (true) {
        xSemaphoreTake(s_led_lock, portMAX_DELAY);
        system_led_effect_t effect = s_effect;

        TickType_t now = xTaskGetTickCount();
        if (!has_active_effect || effect != active_effect) {
            active_effect = effect;
            has_active_effect = true;
            blink_on = true;
            last_toggle_tick = now;
            esp_err_t err = write_effect(active_effect);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "set RGB LED effect failed: %s",
                         esp_err_to_name(err));
            }
        } else if (is_blinking_effect(active_effect)) {
            uint32_t elapsed_ms = pdTICKS_TO_MS(now - last_toggle_tick);
            if (elapsed_ms >= SYSTEM_LED_GREEN_BLINK_MS) {
                blink_on = !blink_on;
                last_toggle_tick = now;
                esp_err_t err = write_effect(
                        blink_on ? active_effect : SYSTEM_LED_EFFECT_OFF);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "blink RGB LED failed: %s",
                             esp_err_to_name(err));
                }
            }
        }
        xSemaphoreGive(s_led_lock);

        vTaskDelay(pdMS_TO_TICKS(SYSTEM_LED_TASK_PERIOD_MS));
    }
}

esp_err_t system_led_init(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = SYSTEM_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = SYSTEM_LED_RESOLUTION_HZ,
        .mem_block_symbols = SYSTEM_LED_MEM_BLOCK_SYMBOLS,
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config,
                                             &s_led_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create RGB LED strip failed: %s", esp_err_to_name(err));
        s_led_strip = NULL;
        return err;
    }

    s_led_lock = xSemaphoreCreateMutex();
    if (s_led_lock == NULL) {
        ESP_LOGE(TAG, "create RGB LED lock failed");
        s_led_strip = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(system_led_task, "led", 2048, NULL, 4,
                                &s_led_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create RGB LED task failed");
        s_led_strip = NULL;
        s_led_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "RGB LED enabled on GPIO%d", SYSTEM_LED_GPIO);
    return ESP_OK;
}

static esp_err_t set_effect(system_led_effect_t effect) {
    if (s_led_strip == NULL || s_led_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_led_lock, portMAX_DELAY);
    s_effect = effect;
    esp_err_t err = write_effect(effect);
    xSemaphoreGive(s_led_lock);

    return err;
}

esp_err_t system_led_set_off(void) {
    return set_effect(SYSTEM_LED_EFFECT_OFF);
}

esp_err_t system_led_set_green(void) {
    return set_effect(SYSTEM_LED_EFFECT_GREEN);
}

esp_err_t system_led_set_green_blink(void) {
    return set_effect(SYSTEM_LED_EFFECT_GREEN_BLINK);
}

esp_err_t system_led_set_blue(void) {
    return set_effect(SYSTEM_LED_EFFECT_BLUE);
}

esp_err_t system_led_set_red(void) {
    return set_effect(SYSTEM_LED_EFFECT_RED);
}
