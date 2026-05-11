idf_component_register(
    SRCS
        "button_manager.c"
        "main.c"
        "nvs_config.c"
        "web_server.c"
        "wifi_manager.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_event
        esp_driver_gpio
        esp_http_server
        esp_netif
        esp_system
        esp_wifi
        nvs_flash
)
