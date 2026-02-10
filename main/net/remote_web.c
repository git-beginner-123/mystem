#include "remote_web.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "nvs_flash.h"

static const char* TAG = "remote_web";

static httpd_handle_t s_httpd = NULL;
static esp_netif_t* s_ap_netif = NULL;
static bool s_wifi_inited = false;

static volatile RemoteCmd s_last_cmd = kRemoteCmdNone;
static volatile bool s_display_on = true;

static void set_cmd(RemoteCmd cmd)
{
    s_last_cmd = cmd;
    if (cmd == kRemoteCmdStart) s_display_on = true;
    if (cmd == kRemoteCmdStop) s_display_on = false;
}

static esp_err_t root_get_handler(httpd_req_t* req)
{
    const char* html =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32 RC</title></head><body>"
        "<h2>ESP32 RC</h2>"
        "<div style='display:flex;flex-direction:column;gap:10px;max-width:220px'>"
        "<button style='height:54px;font-size:18px' onclick=\"send('up')\">UP</button>"
        "<div style='display:flex;gap:10px'>"
        "<button style='flex:1;height:54px;font-size:18px' onclick=\"send('left')\">LEFT</button>"
        "<button style='flex:1;height:54px;font-size:18px' onclick=\"send('right')\">RIGHT</button>"
        "</div>"
        "<button style='height:54px;font-size:18px' onclick=\"send('down')\">DOWN</button>"
        "<div style='display:flex;gap:10px'>"
        "<button style='flex:1;height:54px;font-size:18px' onclick=\"send('start')\">START</button>"
        "<button style='flex:1;height:54px;font-size:18px' onclick=\"send('stop')\">STOP</button>"
        "</div>"
        "</div>"
        "<p id='s'>Status: ready</p>"
        "<script>"
        "function setText(t){document.getElementById('s').innerText=t;}"
        "function send(cmd){fetch('/cmd/'+cmd).then(()=>setText('Status: '+cmd));}"
        "</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t cmd_up_handler(httpd_req_t* req)
{
    set_cmd(kRemoteCmdUp);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK\n", HTTPD_RESP_USE_STRLEN);
}
static esp_err_t cmd_down_handler(httpd_req_t* req)
{
    set_cmd(kRemoteCmdDown);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK\n", HTTPD_RESP_USE_STRLEN);
}
static esp_err_t cmd_left_handler(httpd_req_t* req)
{
    set_cmd(kRemoteCmdLeft);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK\n", HTTPD_RESP_USE_STRLEN);
}
static esp_err_t cmd_right_handler(httpd_req_t* req)
{
    set_cmd(kRemoteCmdRight);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK\n", HTTPD_RESP_USE_STRLEN);
}
static esp_err_t cmd_start_handler(httpd_req_t* req)
{
    set_cmd(kRemoteCmdStart);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK\n", HTTPD_RESP_USE_STRLEN);
}
static esp_err_t cmd_stop_handler(httpd_req_t* req)
{
    set_cmd(kRemoteCmdStop);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK\n", HTTPD_RESP_USE_STRLEN);
}

static bool web_start(void)
{
    if (s_httpd) return true;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 4096;
    cfg.max_uri_handlers = 8;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        s_httpd = NULL;
        ESP_LOGE(TAG, "httpd_start failed");
        return false;
    }

    httpd_uri_t root  = {.uri="/", .method=HTTP_GET, .handler=root_get_handler, .user_ctx=NULL};
    httpd_uri_t up    = {.uri="/cmd/up", .method=HTTP_GET, .handler=cmd_up_handler, .user_ctx=NULL};
    httpd_uri_t down  = {.uri="/cmd/down", .method=HTTP_GET, .handler=cmd_down_handler, .user_ctx=NULL};
    httpd_uri_t left  = {.uri="/cmd/left", .method=HTTP_GET, .handler=cmd_left_handler, .user_ctx=NULL};
    httpd_uri_t right = {.uri="/cmd/right", .method=HTTP_GET, .handler=cmd_right_handler, .user_ctx=NULL};
    httpd_uri_t start = {.uri="/cmd/start", .method=HTTP_GET, .handler=cmd_start_handler, .user_ctx=NULL};
    httpd_uri_t stop  = {.uri="/cmd/stop", .method=HTTP_GET, .handler=cmd_stop_handler, .user_ctx=NULL};

    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &up);
    httpd_register_uri_handler(s_httpd, &down);
    httpd_register_uri_handler(s_httpd, &left);
    httpd_register_uri_handler(s_httpd, &right);
    httpd_register_uri_handler(s_httpd, &start);
    httpd_register_uri_handler(s_httpd, &stop);

    ESP_LOGI(TAG, "web started");
    return true;
}

static void web_stop(void)
{
    if (!s_httpd) return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
    ESP_LOGI(TAG, "web stopped");
}

static bool ap_start(void)
{
    if (!s_wifi_inited) {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ESP_ERROR_CHECK(nvs_flash_init());
        }

        ESP_ERROR_CHECK(esp_netif_init());

        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(err);
        }

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        s_wifi_inited = true;
    }

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap_cfg = {0};
    strncpy((char*)ap_cfg.ap.ssid, "ESP32_RC", sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "ap started SSID=ESP32_RC ip=192.168.4.1");
    return true;
}

static void ap_stop(void)
{
    esp_wifi_stop();
}

bool RemoteWeb_Start(void)
{
    if (!ap_start()) return false;
    if (!web_start()) return false;
    return true;
}

void RemoteWeb_Stop(void)
{
    web_stop();
    ap_stop();
    s_last_cmd = kRemoteCmdNone;
    s_display_on = true;
}

RemoteCmd RemoteWeb_PopCmd(void)
{
    RemoteCmd cmd = s_last_cmd;
    s_last_cmd = kRemoteCmdNone;
    return cmd;
}

bool RemoteWeb_DisplayOn(void)
{
    return s_display_on;
}
