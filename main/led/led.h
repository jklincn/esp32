#pragma once

#include "esp_err.h"

typedef enum {
    SYSTEM_LED_EFFECT_OFF,
    SYSTEM_LED_EFFECT_GREEN,
    SYSTEM_LED_EFFECT_GREEN_BLINK,
    SYSTEM_LED_EFFECT_BLUE,
    SYSTEM_LED_EFFECT_BLUE_BLINK,
    SYSTEM_LED_EFFECT_RED,
} system_led_effect_t;

esp_err_t system_led_init(void);
esp_err_t system_led_set(system_led_effect_t effect);
