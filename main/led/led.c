#include "led/led.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"

#define STATUS_LED_GPIO GPIO_NUM_8
#define STATUS_LED_RESOLUTION_HZ 10000000
#define STATUS_LED_MEM_BLOCK_SYMBOLS 64
#define STATUS_LED_BRIGHTNESS 32
#define STATUS_LED_TASK_PERIOD_MS 50
#define STATUS_LED_NORMAL_BLINK_MS 600
#define STATUS_LED_CLEAR_READY_BLINK_MS 300

static const char *TAG = "[led]";

typedef enum {
    STATUS_LED_EFFECT_OFF,
    STATUS_LED_EFFECT_NORMAL_SOLID,
    STATUS_LED_EFFECT_NORMAL_BLINK,
    STATUS_LED_EFFECT_ERROR_SOLID,
    STATUS_LED_EFFECT_CLEAR_READY_BLINK,
} status_led_effect_t;

static led_strip_handle_t s_led_strip;
static SemaphoreHandle_t s_led_lock;
static TaskHandle_t s_led_task;
static bool s_initialized;
static bool s_led_enabled;
static bool s_normal_blinking;
static bool s_clear_ready;
static bool s_error;

static esp_err_t write_rgb(uint8_t red, uint8_t green, uint8_t blue) {
    esp_err_t err = led_strip_set_pixel(s_led_strip, 0, red, green, blue);
    if (err != ESP_OK) {
        return err;
    }

    return led_strip_refresh(s_led_strip);
}

static status_led_effect_t get_effect_locked(void) {
    if (!s_led_enabled) {
        return STATUS_LED_EFFECT_OFF;
    }

    if (s_clear_ready) {
        return STATUS_LED_EFFECT_CLEAR_READY_BLINK;
    }

    if (s_error) {
        return STATUS_LED_EFFECT_ERROR_SOLID;
    }

    return s_normal_blinking ? STATUS_LED_EFFECT_NORMAL_BLINK
                             : STATUS_LED_EFFECT_NORMAL_SOLID;
}

static esp_err_t write_effect(status_led_effect_t effect, bool on) {
    if (!on || effect == STATUS_LED_EFFECT_OFF) {
        return write_rgb(0, 0, 0);
    }

    if (effect == STATUS_LED_EFFECT_CLEAR_READY_BLINK) {
        return write_rgb(0, 0, STATUS_LED_BRIGHTNESS);
    }

    if (effect == STATUS_LED_EFFECT_ERROR_SOLID) {
        return write_rgb(STATUS_LED_BRIGHTNESS, 0, 0);
    }

    return write_rgb(0, STATUS_LED_BRIGHTNESS, 0);
}

static uint32_t effect_period_ms(status_led_effect_t effect) {
    if (effect == STATUS_LED_EFFECT_CLEAR_READY_BLINK) {
        return STATUS_LED_CLEAR_READY_BLINK_MS;
    }

    return STATUS_LED_NORMAL_BLINK_MS;
}

static bool is_blinking_effect(status_led_effect_t effect) {
    return effect == STATUS_LED_EFFECT_NORMAL_BLINK ||
           effect == STATUS_LED_EFFECT_CLEAR_READY_BLINK;
}

static void status_led_task(void *arg) {
    (void)arg;

    status_led_effect_t active_effect = STATUS_LED_EFFECT_OFF;
    bool has_active_effect = false;
    bool blink_on = true;
    TickType_t last_toggle_tick = 0;

    while (true) {
        xSemaphoreTake(s_led_lock, portMAX_DELAY);
        status_led_effect_t effect = get_effect_locked();

        TickType_t now = xTaskGetTickCount();
        if (!has_active_effect || effect != active_effect) {
            active_effect = effect;
            has_active_effect = true;
            blink_on = true;
            last_toggle_tick = now;
            esp_err_t err = write_effect(active_effect, true);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "set RGB LED effect failed: %s",
                         esp_err_to_name(err));
            }
        } else if (is_blinking_effect(active_effect)) {
            uint32_t elapsed_ms = pdTICKS_TO_MS(now - last_toggle_tick);
            if (elapsed_ms >= effect_period_ms(active_effect)) {
                blink_on = !blink_on;
                last_toggle_tick = now;
                esp_err_t err = write_effect(active_effect, blink_on);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "blink RGB LED failed: %s",
                             esp_err_to_name(err));
                }
            }
        }
        xSemaphoreGive(s_led_lock);

        vTaskDelay(pdMS_TO_TICKS(STATUS_LED_TASK_PERIOD_MS));
    }
}

esp_err_t status_led_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = STATUS_LED_RESOLUTION_HZ,
        .mem_block_symbols = STATUS_LED_MEM_BLOCK_SYMBOLS,
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

    BaseType_t ok = xTaskCreate(status_led_task, "led", 2048, NULL, 4,
                                &s_led_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create RGB LED task failed");
        s_led_strip = NULL;
        s_led_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "RGB LED enabled on GPIO%d", STATUS_LED_GPIO);
    return ESP_OK;
}

esp_err_t status_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_led_lock, portMAX_DELAY);
    esp_err_t err = write_rgb(red, green, blue);
    xSemaphoreGive(s_led_lock);
    return err;
}

esp_err_t status_led_set_normal(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_led_lock, portMAX_DELAY);
    s_led_enabled = true;
    s_error = false;
    s_clear_ready = false;
    status_led_effect_t effect = get_effect_locked();
    esp_err_t err = write_effect(effect, true);
    xSemaphoreGive(s_led_lock);

    return err;
}

esp_err_t status_led_set_normal_blinking(bool enabled) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_led_lock, portMAX_DELAY);
    s_led_enabled = true;
    s_error = false;
    s_normal_blinking = enabled;
    status_led_effect_t effect = get_effect_locked();
    esp_err_t err = write_effect(effect, true);
    xSemaphoreGive(s_led_lock);

    return err;
}

esp_err_t status_led_set_error(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_led_lock, portMAX_DELAY);
    s_led_enabled = true;
    s_error = true;
    s_clear_ready = false;
    status_led_effect_t effect = get_effect_locked();
    esp_err_t err = write_effect(effect, true);
    xSemaphoreGive(s_led_lock);

    return err;
}

esp_err_t status_led_set_clear_ready(bool on) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_led_lock, portMAX_DELAY);
    s_led_enabled = true;
    s_clear_ready = on;
    status_led_effect_t effect = get_effect_locked();
    esp_err_t err = write_effect(effect, true);
    xSemaphoreGive(s_led_lock);

    return err;
}
