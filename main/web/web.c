#include "web/web.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "storage/storage.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi/wifi.h"

/* POST /api/wifi_config 的表单体上限。当前只包含 ssid/password，256
 * 字节足够且可防止异常大请求占内存。 */
#define WIFI_FORM_MAX_BODY_LEN 256

/* 配网成功后留一点时间把 HTTP 响应发回浏览器，再重启进入正常启动路径。 */
#define WIFI_CONFIG_RESTART_DELAY_MS 1500

/* SSID JSON 转义后的最坏情况：每个字节写成 \u00XX。 */
#define WIFI_SCAN_ESCAPED_SSID_LEN (WIFI_CFG_MAX_SSID_LEN * 6 + 1)

static const char *TAG = "[web]";

extern const uint8_t web_config_html_start[] asm("_binary_config_html_start");
extern const uint8_t web_normal_html_start[] asm("_binary_normal_html_start");

/* HTTP server 句柄；非 NULL 表示服务已经启动。 */
static httpd_handle_t s_server;
static web_server_mode_t s_mode;

static esp_err_t fail_start(esp_err_t err) {
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return err;
}

/*
 * Wi-Fi 配置请求的异步任务参数。
 * httpd_req_t 通过 httpd_req_async_handler_begin()
 * 延长生命周期，任务完成后必须调用 httpd_req_async_handler_complete() 归还给
 * HTTP server。
 */
typedef struct {
    httpd_req_t *req;
    wifi_cfg_t wifi;
} wifi_config_job_t;

/* 发送 JSON 响应，并把常用状态码转换为 ESP HTTP server 需要的状态字符串。 */
static void send_json(httpd_req_t *req, int status_code, const char *json) {
    const char *status = "200 OK";
    if (status_code == 400) {
        status = "400 Bad Request";
    } else if (status_code == 409) {
        status = "409 Conflict";
    } else if (status_code == 500) {
        status = "500 Internal Server Error";
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "send JSON response failed: %s", esp_err_to_name(err));
    }
}

/* 把单个十六进制字符转换为 0..15；非法字符返回 -1。 */
static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/*
 * 解码 application/x-www-form-urlencoded 中的字段值。
 * '+' 代表空格，'%XX' 代表一个字节；输出缓冲区不够时返回 false。
 */
static bool url_decode(const char *src, char *dst, size_t dst_len) {
    size_t out = 0;
    for (size_t i = 0; src[i] != '\0'; i++) {
        if (out + 1 >= dst_len) {
            return false;
        }

        if (src[i] == '+') {
            dst[out++] = ' ';
        } else if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) &&
                   isxdigit((unsigned char)src[i + 2])) {
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);
            dst[out++] = (char)((hi << 4) | lo);
            i += 2;
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
    return true;
}

static bool json_escape_string(const char *src, char *dst, size_t dst_len) {
    size_t out = 0;
    for (size_t i = 0; src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];

        if (c == '"' || c == '\\') {
            if (out + 2 >= dst_len) {
                return false;
            }
            dst[out++] = '\\';
            dst[out++] = (char)c;
        } else if (c < 0x20) {
            if (out + 6 >= dst_len) {
                return false;
            }
            snprintf(dst + out, dst_len - out, "\\u%04x", c);
            out += 6;
        } else {
            if (out + 1 >= dst_len) {
                return false;
            }
            dst[out++] = (char)c;
        }
    }

    dst[out] = '\0';
    return true;
}

static bool scan_refresh_requested(httpd_req_t *req) {
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0 || query_len >= 64) {
        return false;
    }

    char query[64];
    esp_err_t err = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (err != ESP_OK) {
        return false;
    }

    char value[8];
    err = httpd_query_key_value(query, "refresh", value, sizeof(value));
    return err == ESP_OK &&
           (strcmp(value, "1") == 0 || strcmp(value, "true") == 0);
}

/*
 * 解析 Wi-Fi 配置表单。
 * body 会被 strtok_r()
 * 原地切分，因此调用方传入的缓冲区不应再作为原始字符串使用。
 */
static bool parse_wifi_form(char *body, char *ssid, size_t ssid_len,
                            char *password, size_t password_len) {
    ssid[0] = '\0';
    password[0] = '\0';

    /* 表单格式形如 ssid=xxx&password=yyy；未知字段直接忽略，便于以后扩展。 */
    char *save_ptr = NULL;
    for (char *pair = strtok_r(body, "&", &save_ptr); pair != NULL;
         pair = strtok_r(NULL, "&", &save_ptr)) {
        char *eq = strchr(pair, '=');
        if (eq == NULL) {
            continue;
        }

        *eq = '\0';
        const char *key = pair;
        const char *value = eq + 1;

        if (strcmp(key, "ssid") == 0) {
            if (!url_decode(value, ssid, ssid_len)) {
                ESP_LOGW(TAG, "SSID is too long after URL decode");
                return false;
            }
        } else if (strcmp(key, "password") == 0) {
            if (!url_decode(value, password, password_len)) {
                ESP_LOGW(TAG, "password is too long after URL decode");
                return false;
            }
        }
    }

    /* SSID 必须非空；password 允许为空，用于开放 Wi-Fi。 */
    return ssid[0] != '\0';
}

/* GET /：返回当前启动模式对应的内置 HTML 页面。 */
static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    const char *html = s_mode == WEB_SERVER_MODE_CONFIG
                           ? (const char *)web_config_html_start
                           : (const char *)web_normal_html_start;
    esp_err_t err = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "send root page failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* GET /api/status：返回当前 Wi-Fi 状态，供页面轮询刷新。 */
static esp_err_t status_handler(httpd_req_t *req) {
    wifi_manager_status_t status;
    wifi_manager_get_status(&status);

    char json[160];
    snprintf(json, sizeof(json),
             "{\"mode\":\"%s\",\"sta_connected\":%s,\"sta_ip\":\"%s\"}",
             status.mode == WIFI_MODE_APSTA ? "config" : "sta",
             status.sta_connected ? "true" : "false", status.sta_ip);

    send_json(req, 200, json);
    return ESP_OK;
}

/* GET /api/wifi_scan：返回扫描缓存；refresh=1 时触发后台刷新。 */
static esp_err_t wifi_scan_handler(httpd_req_t *req) {
    bool refresh = scan_refresh_requested(req);
    if (refresh) {
        esp_err_t err = wifi_manager_request_scan();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "request Wi-Fi scan failed: %s",
                     esp_err_to_name(err));
            send_json(req, 500,
                      "{\"ok\":false,\"message\":\"Failed to start Wi-Fi "
                      "scan\"}");
            return ESP_OK;
        }
    }

    wifi_scan_snapshot_t snapshot;
    wifi_manager_get_scan_snapshot(&snapshot);
    if (!snapshot.valid && !snapshot.scanning) {
        esp_err_t err = wifi_manager_request_scan();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "request initial Wi-Fi scan failed: %s",
                     esp_err_to_name(err));
            send_json(req, 500,
                      "{\"ok\":false,\"message\":\"Failed to start Wi-Fi "
                      "scan\"}");
            return ESP_OK;
        }
        wifi_manager_get_scan_snapshot(&snapshot);
    }

    const char *last_error = snapshot.last_error == ESP_OK
                                 ? "ESP_OK"
                                 : esp_err_to_name(snapshot.last_error);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char head[192];
    snprintf(head, sizeof(head),
             "{\"ok\":true,\"valid\":%s,\"scanning\":%s,\"age_ms\":%lu,"
             "\"last_error\":\"%s\",\"networks\":[",
             snapshot.valid ? "true" : "false",
             snapshot.scanning ? "true" : "false",
             (unsigned long)snapshot.age_ms, last_error);
    esp_err_t err = httpd_resp_sendstr_chunk(req, head);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "send scan JSON header failed: %s",
                 esp_err_to_name(err));
        return ESP_OK;
    }

    uint16_t sent_count = 0;
    for (uint16_t i = 0; i < snapshot.ap_count; i++) {
        char escaped_ssid[WIFI_SCAN_ESCAPED_SSID_LEN];
        if (!json_escape_string(snapshot.aps[i].ssid, escaped_ssid,
                                sizeof(escaped_ssid))) {
            ESP_LOGW(TAG, "skip SSID that is too long after JSON escape");
            continue;
        }

        char item[320];
        snprintf(item, sizeof(item),
                 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%u,"
                 "\"password_required\":%s}",
                 sent_count == 0 ? "" : ",", escaped_ssid,
                 snapshot.aps[i].rssi, snapshot.aps[i].channel,
                 snapshot.aps[i].password_required ? "true" : "false");
        err = httpd_resp_sendstr_chunk(req, item);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "send scan JSON item failed: %s",
                     esp_err_to_name(err));
            return ESP_OK;
        }
        sent_count++;
    }

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/*
 * Wi-Fi 验证任务。
 * 连接路由器最多可能阻塞十几秒，不能直接在 HTTP server 工作线程里执行，
 * 否则会影响其他请求处理；因此 handler 只读取请求并创建该任务。
 */
static void wifi_config_task(void *arg) {
    wifi_config_job_t *job = (wifi_config_job_t *)arg;
    bool restart_after_response = false;

    /*
     * Wi-Fi manager 只负责连接验证；验证成功后由这里保存 NVS 并关闭配网页面。
     * 任务里根据错误类型返回不同 JSON，前端据此显示成功、失败或并发冲突。
     */
    wifi_manager_connect_result_t result;
    esp_err_t err = wifi_manager_try_connect(&job->wifi, &result);
    bool connected = err == ESP_OK && result.ok;

    if (connected) {
        err = app_nvs_save_wifi(job->wifi.ssid, job->wifi.password);
    }

    if (connected && err == ESP_OK) {
        char json[128];
        snprintf(json, sizeof(json),
                 "{\"ok\":true,\"message\":\"Wi-Fi connected\","
                 "\"ip\":\"%s\"}",
                 result.ip);
        send_json(job->req, 200, json);
        httpd_req_async_handler_complete(job->req);
        restart_after_response = true;
    } else if (connected) {
        ESP_LOGE(TAG, "save verified Wi-Fi config failed: %s",
                 esp_err_to_name(err));
        send_json(job->req, 500,
                  "{\"ok\":false,\"message\":\"Failed to save Wi-Fi config\"}");
        httpd_req_async_handler_complete(job->req);
    } else if (err == ESP_ERR_INVALID_STATE) {
        send_json(
            job->req, 409,
            "{\"ok\":false,\"message\":\"Wi-Fi validation already running\"}");
        httpd_req_async_handler_complete(job->req);
    } else {
        send_json(job->req, 200,
                  "{\"ok\":false,\"message\":\"Wi-Fi connection failed\"}");
        httpd_req_async_handler_complete(job->req);
    }

    free(job);
    if (restart_after_response) {
        ESP_LOGI(TAG, "restarting after Wi-Fi config saved");
        vTaskDelay(pdMS_TO_TICKS(WIFI_CONFIG_RESTART_DELAY_MS));
        esp_restart();
    }

    vTaskDelete(NULL);
}

/* POST /api/wifi_config：读取表单、创建异步任务验证并保存 Wi-Fi 配置。 */
static esp_err_t wifi_config_handler(httpd_req_t *req) {
    /* 拒绝空请求和过大的请求，避免无意义解析或栈上缓冲区溢出。 */
    if (req->content_len <= 0 || req->content_len >= WIFI_FORM_MAX_BODY_LEN) {
        ESP_LOGW(TAG, "invalid wifi_config body length=%d", req->content_len);
        send_json(req, 400,
                  "{\"ok\":false,\"message\":\"Invalid request body\"}");
        return ESP_OK;
    }

    /*
     * httpd_req_recv() 可能一次读不完完整 body，因此按 remaining 循环读取。
     * 末尾额外补 '\0'，方便后续按 C 字符串解析。
     */
    char body[WIFI_FORM_MAX_BODY_LEN] = {0};
    int received = 0;
    int remaining = req->content_len;
    while (remaining > 0) {
        int ret = httpd_req_recv(req, body + received, remaining);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            ESP_LOGE(TAG, "receive wifi_config body failed, ret=%d", ret);
            send_json(req, 500,
                      "{\"ok\":false,\"message\":\"Failed to read request\"}");
            return ESP_OK;
        }
        received += ret;
        remaining -= ret;
    }
    body[received] = '\0';

    /* job 在异步任务结束时释放；如果创建任务失败，则在当前 handler 中释放。 */
    wifi_config_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        ESP_LOGE(TAG, "alloc wifi_config job failed");
        send_json(req, 500, "{\"ok\":false,\"message\":\"Out of memory\"}");
        return ESP_OK;
    }

    if (!parse_wifi_form(body, job->wifi.ssid, sizeof(job->wifi.ssid),
                         job->wifi.password, sizeof(job->wifi.password))) {
        ESP_LOGW(TAG, "invalid wifi_config form");
        free(job);
        send_json(req, 400,
                  "{\"ok\":false,\"message\":\"Invalid SSID\"}");
        return ESP_OK;
    }
    job->wifi.initialized = true;

    ESP_LOGI(TAG, "received wifi_config request, ssid=%s", job->wifi.ssid);

    /*
     * 开启异步请求后，HTTP server 允许当前 handler 返回，后续由任务发送响应并
     * complete。
     */
    esp_err_t err = httpd_req_async_handler_begin(req, &job->req);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "begin async HTTP request failed: %s",
                 esp_err_to_name(err));
        free(job);
        send_json(req, 500,
                  "{\"ok\":false,\"message\":\"Async request failed\"}");
        return ESP_OK;
    }

    /* 单独任务栈稍大一些，给 Wi-Fi 验证、JSON 拼接和日志留余量。 */
    BaseType_t ok = xTaskCreate(wifi_config_task, "wifi_cfg_task", 6144, job, 4,
                                NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create wifi_config task failed");
        send_json(
            job->req, 500,
            "{\"ok\":false,\"message\":\"Failed to start Wi-Fi validation\"}");
        httpd_req_async_handler_complete(job->req);
        free(job);
    }

    return ESP_OK;
}

esp_err_t web_server_start(web_server_mode_t mode) {
    /* 防止重复启动 HTTP server；重复调用视为成功。 */
    if (s_server != NULL) {
        ESP_LOGW(TAG, "HTTP server already started");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* 启用 LRU 清理可在连接资源紧张时回收旧连接；栈大小按当前 handler
     * 复杂度提高。 */
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    config.max_uri_handlers = 8;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_mode = mode;

    /* 路由表使用局部 const 结构体注册；注册后 ESP HTTP server 会复制必要信息。
     */
    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &root_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register / failed: %s", esp_err_to_name(err));
        return fail_start(err);
    }
    err = httpd_register_uri_handler(s_server, &status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /api/status failed: %s", esp_err_to_name(err));
        return fail_start(err);
    }

    if (mode == WEB_SERVER_MODE_CONFIG) {
        const httpd_uri_t wifi_config_uri = {
            .uri = "/api/wifi_config",
            .method = HTTP_POST,
            .handler = wifi_config_handler,
            .user_ctx = NULL,
        };
        const httpd_uri_t wifi_scan_uri = {
            .uri = "/api/wifi_scan",
            .method = HTTP_GET,
            .handler = wifi_scan_handler,
            .user_ctx = NULL,
        };
        err = httpd_register_uri_handler(s_server, &wifi_config_uri);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register /api/wifi_config failed: %s",
                     esp_err_to_name(err));
            return fail_start(err);
        }
        err = httpd_register_uri_handler(s_server, &wifi_scan_uri);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register /api/wifi_scan failed: %s",
                     esp_err_to_name(err));
            return fail_start(err);
        }
    }
    ESP_LOGI(TAG, "HTTP server started on port %d in %s mode",
             config.server_port,
             mode == WEB_SERVER_MODE_CONFIG ? "config" : "normal");

    return ESP_OK;
}
