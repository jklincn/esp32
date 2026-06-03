set(SMS_GATEWAY_API_URL_VALUE "")
set(SMS_GATEWAY_API_TOKEN_VALUE "")

function(strip_optional_quotes INPUT OUTPUT)
    set(VALUE "${INPUT}")
    string(STRIP "${VALUE}" VALUE)
    if(VALUE MATCHES "^\"(.*)\"$")
        set(VALUE "${CMAKE_MATCH_1}")
    elseif(VALUE MATCHES "^'(.*)'$")
        set(VALUE "${CMAKE_MATCH_1}")
    endif()
    set(${OUTPUT} "${VALUE}" PARENT_SCOPE)
endfunction()

get_filename_component(PROJECT_ENV_FILE "${CMAKE_CURRENT_LIST_DIR}/../.env"
                       ABSOLUTE)
if(EXISTS "${PROJECT_ENV_FILE}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${PROJECT_ENV_FILE}")
    file(STRINGS "${PROJECT_ENV_FILE}" PROJECT_ENV_LINES)
    foreach(PROJECT_ENV_LINE IN LISTS PROJECT_ENV_LINES)
        string(STRIP "${PROJECT_ENV_LINE}" PROJECT_ENV_LINE)
        if(PROJECT_ENV_LINE MATCHES "^(export[ \t]+)?SMS_GATEWAY_API_URL=(.*)$")
            strip_optional_quotes("${CMAKE_MATCH_2}" DOTENV_API_URL)
            if(NOT "${DOTENV_API_URL}" STREQUAL "")
                set(SMS_GATEWAY_API_URL_VALUE "${DOTENV_API_URL}")
            endif()
        elseif(PROJECT_ENV_LINE MATCHES "^(export[ \t]+)?SMS_GATEWAY_API_TOKEN=(.*)$")
            strip_optional_quotes("${CMAKE_MATCH_2}" DOTENV_API_TOKEN)
            if(NOT "${DOTENV_API_TOKEN}" STREQUAL "")
                set(SMS_GATEWAY_API_TOKEN_VALUE "${DOTENV_API_TOKEN}")
            endif()
        endif()
    endforeach()
endif()

if(DEFINED ENV{SMS_GATEWAY_API_URL} AND NOT "$ENV{SMS_GATEWAY_API_URL}" STREQUAL "")
    set(SMS_GATEWAY_API_URL_VALUE "$ENV{SMS_GATEWAY_API_URL}")
    message(STATUS "SMS Gateway API URL loaded from environment")
endif()

if(DEFINED ENV{SMS_GATEWAY_API_TOKEN} AND NOT "$ENV{SMS_GATEWAY_API_TOKEN}" STREQUAL "")
    set(SMS_GATEWAY_API_TOKEN_VALUE "$ENV{SMS_GATEWAY_API_TOKEN}")
    message(STATUS "SMS Gateway API token loaded from environment")
endif()

function(escape_c_string INPUT OUTPUT)
    set(ESCAPED_VALUE "${INPUT}")
    string(REPLACE "\\" "\\\\" ESCAPED_VALUE "${ESCAPED_VALUE}")
    string(REPLACE "\"" "\\\"" ESCAPED_VALUE "${ESCAPED_VALUE}")
    string(REPLACE "\n" "\\n" ESCAPED_VALUE "${ESCAPED_VALUE}")
    string(REPLACE "\r" "\\r" ESCAPED_VALUE "${ESCAPED_VALUE}")
    set(${OUTPUT} "${ESCAPED_VALUE}" PARENT_SCOPE)
endfunction()

escape_c_string("${SMS_GATEWAY_API_URL_VALUE}" SMS_GATEWAY_API_URL_C)
escape_c_string("${SMS_GATEWAY_API_TOKEN_VALUE}" SMS_GATEWAY_API_TOKEN_C)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/notify/notify_config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/notify_config.h"
    @ONLY
)

idf_component_register(
    SRCS
        "app/main.c"
        "button/button.c"
        "led/led.c"
        "notify/notify.c"
        "storage/storage.c"
        "web/web.c"
        "wifi/wifi.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"
    EMBED_TXTFILES
        "web/index.html"
    PRIV_REQUIRES
        esp_event
        esp_driver_gpio
        esp_http_client
        esp_http_server
        esp_netif
        esp_system
        esp_wifi
        mbedtls
        nvs_flash
)
