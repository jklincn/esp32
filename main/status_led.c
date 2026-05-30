#include "status_led.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "led_strip.h"

#define STATUS_LED_GPIO GPIO_NUM_8
#define STATUS_LED_RESOLUTION_HZ 10000000
#define STATUS_LED_MEM_BLOCK_SYMBOLS 64
#define STATUS_LED_BRIGHTNESS 32

static const char *TAG = "[status_led]";

static led_strip_handle_t s_led_strip;
static bool s_initialized;

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

    s_initialized = true;
    ESP_LOGI(TAG, "RGB LED enabled on GPIO%d", STATUS_LED_GPIO);
    return ESP_OK;
}

esp_err_t status_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = led_strip_set_pixel(s_led_strip, 0, red, green, blue);
    if (err != ESP_OK) {
        return err;
    }

    return led_strip_refresh(s_led_strip);
}

esp_err_t status_led_set_normal(void) {
    return status_led_set_rgb(0, STATUS_LED_BRIGHTNESS, 0);
}

esp_err_t status_led_set_clear_ready(bool on) {
    if (on) {
        return status_led_set_rgb(0, 0, STATUS_LED_BRIGHTNESS);
    }

    return status_led_set_rgb(0, 0, 0);
}
