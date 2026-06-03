#pragma once

#include "esp_err.h"

esp_err_t system_led_init(void);
esp_err_t system_led_set_off(void);
esp_err_t system_led_set_green(void);
esp_err_t system_led_set_green_blink(void);
esp_err_t system_led_set_blue(void);
esp_err_t system_led_set_red(void);
