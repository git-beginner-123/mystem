#include "remote_web.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"

static const char* TAG = "remote_web";

static httpd_handle_t s_httpd = NULL;
static esp_netif_t* s_ap_netif = NULL;

static void led_set(bool on)
{
    // TODO: connect to your existing WS2812 driver on GPIO48
    // For now: verify web control path first
    ESP_LOGI(TAG, "LED %s", on ? "ON" : "OFF");
}

static esp_err_t root_get_handler(httpd_req_t* req)
{
    const char* html =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>STEM Remote</title></head><body>"
        "<h2>STEM Wi-Fi Remote</h2>"
        "<button style='width:140px;height:60px;font-size:18px' "
        "onclick=\"fetch('/led/on')\">LED ON</button>"
        "<button style='width:140px;height:60px;font-size:18px;margin-left:10px' "
        "onclick=\"fetch('/led/off')\">LED OFF</button>"
        "<p id='s'>Status: ready</p>"
        "<script>"
        "function setText(t){document.getElementById('s').innerText=t;}"
        "document.querySelectorAll('button').forEach(b=>{"
        "b.addEventListener('click',()=>setText('Status: sent'));"
        "});"
        "</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t led_on_handler(httpd_req_t* req)
{
    led_set(true);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK\n", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t led_off_handler(httpd_req_t* req)
{
    led_set(false);
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

    httpd_uri_t root = {.uri="/", .method=HTTP_GET, .handler=root_get_handler, .user_ctx=NULL};
    httpd_uri_t on   = {.uri="/led/on", .method=HTTP_GET, .handler=led_on_handler, .user_ctx=NULL};
    httpd_uri_t off  = {.uri="/led/off", .method=HTTP_GET, .handler=led_off_handler, .user_ctx=NULL};

    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &on);
    httpd_register_uri_handler(s_httpd, &off);

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
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap_cfg = {0};
    strncpy((char*)ap_cfg.ap.ssid, "STEM-REMOTE", sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "ap started SSID=STEM-REMOTE ip=192.168.4.1");
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
    led_set(false);
}
