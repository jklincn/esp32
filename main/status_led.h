#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t status_led_init(void);
esp_err_t status_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue);
esp_err_t status_led_set_normal(void);
esp_err_t status_led_set_normal_blinking(bool enabled);
esp_err_t status_led_set_error(void);
esp_err_t status_led_set_clear_ready(bool on);
