#include "web_server.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_config.h"
#include "wifi_manager.h"

#define WIFI_FORM_MAX_BODY_LEN 256

static const char *TAG = "web_server";
static httpd_handle_t s_server;

typedef struct {
    httpd_req_t *req;
    char ssid[WIFI_CFG_MAX_SSID_LEN + 1];
    char password[WIFI_CFG_MAX_PASSWORD_LEN + 1];
} wifi_config_job_t;

static const char INDEX_HTML[] =
    "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
    "<meta name=\"color-scheme\" content=\"light\">"
    "<title>SMS Gateway</title>"
    "<style>"
    ":root{--bg:#f6f7f9;--panel:#fff;--text:#16202a;--muted:#607080;--line:#d9e0e7;--accent:#0b6bcb;--ok:#0f7b4f;--warn:#b45309;--bad:#b42318}"
    "*{box-sizing:border-box}"
    "body{margin:0;background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Arial,\"Noto Sans SC\",sans-serif;font-size:16px;line-height:1.5}"
    "main{width:min(760px,100%);margin:0 auto;padding:24px}"
    "header{padding:10px 0 18px}"
    "h1{margin:0;font-size:28px;line-height:1.2;font-weight:700;letter-spacing:0}"
    "h2{margin:0 0 14px;font-size:18px;line-height:1.3}"
    ".sub{margin:6px 0 0;color:var(--muted)}"
    ".panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:18px;margin:14px 0;box-shadow:0 1px 2px rgba(16,24,40,.04)}"
    ".grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}"
    ".item{border:1px solid var(--line);border-radius:8px;padding:12px;background:#fbfcfd;min-width:0}"
    ".label{display:block;color:var(--muted);font-size:13px;margin-bottom:4px}"
    ".value{font-weight:650;overflow-wrap:anywhere}"
    ".pill{display:inline-flex;align-items:center;min-height:28px;padding:3px 10px;border-radius:999px;background:#eef4ff;color:#1849a9;font-weight:650}"
    ".pill.ok{background:#e8f6ef;color:var(--ok)}.pill.bad{background:#fff1f0;color:var(--bad)}.pill.warn{background:#fff7ed;color:var(--warn)}"
    "form{margin:0}"
    "label{display:block;margin:14px 0 6px;font-weight:650}"
    "input{width:100%;min-height:44px;border:1px solid #c8d2dc;border-radius:8px;padding:10px 12px;background:#fff;color:var(--text);font:inherit;outline:none}"
    "input:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(11,107,203,.14)}"
    ".actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:18px}"
    "button{min-height:42px;border:1px solid transparent;border-radius:8px;padding:9px 14px;font:inherit;font-weight:650;cursor:pointer}"
    "button.primary{background:var(--accent);color:#fff}button.secondary{background:#fff;border-color:#c8d2dc;color:#27364a}"
    "button:disabled{opacity:.65;cursor:not-allowed}"
    ".msg{margin-top:16px;border:1px solid var(--line);border-radius:8px;padding:12px;background:#fbfcfd;min-height:46px;overflow-wrap:anywhere}"
    ".msg.ok{border-color:#a7dbc1;background:#eefaf4;color:#075e3d}.msg.bad{border-color:#f4b5ad;background:#fff5f4;color:#9f1d13}.msg.warn{border-color:#f7c98b;background:#fff8ed;color:#8a3f00}"
    "a{color:var(--accent);font-weight:650}"
    "@media(max-width:560px){main{padding:18px 14px}.grid{grid-template-columns:1fr}.actions{flex-direction:column}button{width:100%}}"
    "</style></head><body>"
    "<main>"
    "<header><h1>SMS Gateway</h1><p class=\"sub\">Wi-Fi 配网与设备状态</p></header>"
    "<section class=\"panel\" aria-labelledby=\"statusTitle\">"
    "<h2 id=\"statusTitle\">当前状态</h2>"
    "<div class=\"grid\">"
    "<div class=\"item\"><span class=\"label\">模式</span><span id=\"mode\" class=\"pill warn\">读取中</span></div>"
    "<div class=\"item\"><span class=\"label\">STA 连接</span><span id=\"staConnected\" class=\"pill warn\">读取中</span></div>"
    "<div class=\"item\"><span class=\"label\">STA IP</span><span id=\"staIp\" class=\"value\">-</span></div>"
    "<div class=\"item\"><span class=\"label\">SoftAP</span><span id=\"apEnabled\" class=\"pill warn\">读取中</span></div>"
    "</div></section>"
    "<section class=\"panel\" aria-labelledby=\"wifiTitle\">"
    "<h2 id=\"wifiTitle\">Wi-Fi 配置</h2>"
    "<form id=\"wifiForm\" autocomplete=\"on\">"
    "<label for=\"ssid\">SSID</label><input id=\"ssid\" name=\"ssid\" maxlength=\"32\" autocomplete=\"off\" required>"
    "<label for=\"password\">Password</label><input id=\"password\" name=\"password\" type=\"password\" maxlength=\"64\" autocomplete=\"current-password\" required>"
    "<div class=\"actions\"><button id=\"connectBtn\" class=\"primary\" type=\"submit\">连接 Wi-Fi</button></div>"
    "</form>"
    "<div id=\"message\" class=\"msg\" role=\"status\" aria-live=\"polite\">等待操作</div>"
    "</section>"
    "</main>"
    "<script>"
    "const $=id=>document.getElementById(id);"
    "function setPill(el,text,cls){el.className='pill '+cls;el.textContent=text;}"
    "function setMessage(text,cls){const m=$('message');m.className='msg '+(cls||'');m.innerHTML=text;}"
    "async function loadStatus(){"
    "try{"
    "const r=await fetch('/api/status',{cache:'no-store'});"
    "const s=await r.json();"
    "setPill($('mode'),s.mode==='config'?'配网模式':'STA 模式',s.mode==='config'?'warn':'ok');"
    "setPill($('staConnected'),s.sta_connected?'已连接':'未连接',s.sta_connected?'ok':'bad');"
    "$('staIp').textContent=s.sta_ip||'-';"
    "setPill($('apEnabled'),s.ap_enabled?'已开启':'已关闭',s.ap_enabled?'warn':'ok');"
    "}catch(err){setMessage('状态读取失败：'+err,'bad');}"
    "}"
    "document.getElementById('wifiForm').addEventListener('submit',async(e)=>{"
    "e.preventDefault();"
    "const btn=$('connectBtn');"
    "btn.disabled=true;"
    "setMessage('正在连接，请稍候','warn');"
    "const body=new URLSearchParams(new FormData(e.target));"
    "try{"
    "const r=await fetch('/api/wifi_config',{method:'POST',body});"
    "const j=await r.json();"
    "if(j.ok){const u='http://'+j.ip;setMessage('Wi-Fi 已连接，STA IP：'+j.ip+'。请切换回家庭 Wi-Fi 后访问 <a href=\"'+u+'\">'+u+'</a>','ok');}"
    "else{setMessage('连接失败，请检查 Wi-Fi 名称和密码：'+j.message,'bad');}"
    "}catch(err){setMessage('请求失败：'+err,'bad');}"
    "finally{btn.disabled=false;}"
    "loadStatus();"
    "});"
    "loadStatus();setInterval(loadStatus,5000);"
    "</script></body></html>";

static void send_json(httpd_req_t *req, int status_code, const char *json)
{
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

static int hex_value(char c)
{
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

static bool url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t out = 0;
    for (size_t i = 0; src[i] != '\0'; i++) {
        if (out + 1 >= dst_len) {
            return false;
        }

        if (src[i] == '+') {
            dst[out++] = ' ';
        } else if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])) {
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

static bool parse_wifi_form(char *body, char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    ssid[0] = '\0';
    password[0] = '\0';

    char *save_ptr = NULL;
    for (char *pair = strtok_r(body, "&", &save_ptr); pair != NULL; pair = strtok_r(NULL, "&", &save_ptr)) {
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

    return ssid[0] != '\0' && password[0] != '\0';
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "send index page failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    wifi_manager_status_t status;
    wifi_manager_get_status(&status);

    char json[192];
    snprintf(json, sizeof(json),
             "{\"mode\":\"%s\",\"sta_connected\":%s,\"sta_ip\":\"%s\",\"ap_enabled\":%s}",
             status.mode == WIFI_MANAGER_MODE_CONFIG ? "config" : "sta",
             status.sta_connected ? "true" : "false",
             status.sta_ip,
             status.ap_enabled ? "true" : "false");

    send_json(req, 200, json);
    return ESP_OK;
}

static void wifi_config_task(void *arg)
{
    wifi_config_job_t *job = (wifi_config_job_t *)arg;
    wifi_manager_connect_result_t result;
    esp_err_t err = wifi_manager_verify_and_save(job->ssid, job->password, &result);

    if (err == ESP_OK && result.ok) {
        char json[128];
        snprintf(json, sizeof(json),
                 "{\"ok\":true,\"message\":\"Wi-Fi connected\",\"ip\":\"%s\"}",
                 result.ip);
        send_json(job->req, 200, json);
        httpd_req_async_handler_complete(job->req);
        wifi_manager_schedule_ap_shutdown();
    } else if (err == ESP_ERR_INVALID_STATE) {
        send_json(job->req, 409, "{\"ok\":false,\"message\":\"Wi-Fi validation already running\"}");
        httpd_req_async_handler_complete(job->req);
    } else {
        send_json(job->req, 200, "{\"ok\":false,\"message\":\"Wi-Fi connection failed\"}");
        httpd_req_async_handler_complete(job->req);
    }

    free(job);
    vTaskDelete(NULL);
}

static esp_err_t wifi_config_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= WIFI_FORM_MAX_BODY_LEN) {
        ESP_LOGW(TAG, "invalid wifi_config body length=%d", req->content_len);
        send_json(req, 400, "{\"ok\":false,\"message\":\"Invalid request body\"}");
        return ESP_OK;
    }

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
            send_json(req, 500, "{\"ok\":false,\"message\":\"Failed to read request\"}");
            return ESP_OK;
        }
        received += ret;
        remaining -= ret;
    }
    body[received] = '\0';

    wifi_config_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        ESP_LOGE(TAG, "alloc wifi_config job failed");
        send_json(req, 500, "{\"ok\":false,\"message\":\"Out of memory\"}");
        return ESP_OK;
    }

    if (!parse_wifi_form(body, job->ssid, sizeof(job->ssid), job->password, sizeof(job->password))) {
        ESP_LOGW(TAG, "invalid wifi_config form");
        free(job);
        send_json(req, 400, "{\"ok\":false,\"message\":\"Invalid SSID or password\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "received wifi_config request, ssid=%s", job->ssid);

    esp_err_t err = httpd_req_async_handler_begin(req, &job->req);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "begin async HTTP request failed: %s", esp_err_to_name(err));
        free(job);
        send_json(req, 500, "{\"ok\":false,\"message\":\"Async request failed\"}");
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(wifi_config_task, "wifi_cfg_task", 6144, job, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create wifi_config task failed");
        send_json(job->req, 500, "{\"ok\":false,\"message\":\"Failed to start Wi-Fi validation\"}");
        httpd_req_async_handler_complete(job->req);
        free(job);
    }

    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "HTTP server already started");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    config.max_uri_handlers = 8;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t wifi_config_uri = {
        .uri = "/api/wifi_config",
        .method = HTTP_POST,
        .handler = wifi_config_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_server, &index_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register / failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(s_server, &wifi_config_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /api/wifi_config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(s_server, &status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /api/status failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);

    return ESP_OK;
}
