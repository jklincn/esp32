#pragma once

#include "esp_err.h"

esp_err_t server_notify_check_configured(void);
esp_err_t server_notify_startup_connected(void);
esp_err_t server_notify_start_heartbeat(void);
