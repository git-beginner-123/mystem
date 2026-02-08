// components/experiments/exp_wifi_remote.c

#include "experiment.h"
#include "ui/ui.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"

#include <string.h>
#include <stdlib.h>

static const char* TAG = "exp_wifi_remote";

/* ---------- Web page (MVP) ---------- */

static const char* k_index_html =
"<!doctype html><html><head><meta charset='utf-8'/>"
"<meta name='viewport' content='width=device-width,initial-scale=1'/>"
"<title>ESP32 Remote</title>"
"<style>"
"body{font-family:system-ui,Arial;margin:16px} .row{display:flex;gap:10px;flex-wrap:wrap}"
"button{font-size:18px;padding:14px 18px} input[type=range]{width:280px}"
".card{border:1px solid #ddd;border-radius:12px;padding:12px;margin:10px 0}"
"</style></head><body>"
"<h2>ESP32 WiFi Remote</h2>"
"<div class='card'>"
"<div>WS: <span id='ws'>disconnected</span></div>"
"<div>Throttle: <span id='t'>0</span> Steer: <span id='s'>0</span></div>"
"</div>"

"<div class='card'>"
"<div>Throttle</div>"
"<input id='th' type='range' min='-100' max='100' value='0'/>"
"</div>"

"<div class='card'>"
"<div>Steer</div>"
"<input id='st' type='range' min='-100' max='100' value='0'/>"
"</div>"

"<div class='row'>"
"<button id='stop'>STOP</button>"
"<button id='zero'>ZERO</button>"
"</div>"

"<script>"
"let ws; const elWs=document.getElementById('ws');"
"const elT=document.getElementById('t'); const elS=document.getElementById('s');"
"const th=document.getElementById('th'); const st=document.getElementById('st');"

"function sendDrive(){"
"  const tv=parseInt(th.value,10); const sv=parseInt(st.value,10);"
"  elT.textContent=tv; elS.textContent=sv;"
"  if(ws && ws.readyState===1){ ws.send(`D ${tv} ${sv}`); }"
"}"

"function connect(){"
"  const url = `ws://${location.host}/ws`;"
"  ws = new WebSocket(url);"
"  ws.onopen=()=>{elWs.textContent='connected'; sendDrive();};"
"  ws.onclose=()=>{elWs.textContent='disconnected'; setTimeout(connect,800);};"
"  ws.onerror=()=>{try{ws.close();}catch(e){}};"
"  ws.onmessage=(ev)=>{/* optional telemetry */};"
"}"

"th.addEventListener('input',sendDrive);"
"st.addEventListener('input',sendDrive);"
"document.getElementById('stop').onclick=()=>{ if(ws&&ws.readyState===1) ws.send('STOP'); };"
"document.getElementById('zero').onclick=()=>{ th.value=0; st.value=0; sendDrive(); };"
"connect();"
"</script></body></html>";

/* ---------- Runtime state ---------- */

typedef struct {
    httpd_handle_t httpd;
    int client_count;
    int throttle;
    int steer;
    bool started;
} wifi_remote_state_t;

static wifi_remote_state_t s_state;

/* ---------- WiFi AP ---------- */

static esp_err_t wifi_init_ap(const char* ssid, const char* pass)
{
    esp_err_t err;

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = (uint8_t)strlen(ssid);
    strncpy((char*)wifi_config.ap.password, pass, sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    if (strlen(pass) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) return err;

    err = esp_wifi_start();
    return err;
}

/* ---------- HTTP handlers ---------- */

static esp_err_t http_root_get(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, k_index_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void ws_send_text(httpd_handle_t hd, int fd, const char* text)
{
    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t*)text;
    frame.len = strlen(text);
    httpd_ws_send_frame_async(hd, fd, &frame);
}

static void parse_drive_line(const char* line, int* out_throttle, int* out_steer)
{
    // Expected: "D <throttle> <steer>"
    int t = 0, s = 0;
    if (sscanf(line, "D %d %d", &t, &s) == 2) {
        if (t > 100) t = 100;
        if (t < -100) t = -100;
        if (s > 100) s = 100;
        if (s < -100) s = -100;
        *out_throttle = t;
        *out_steer = s;
    }
}

static esp_err_t http_ws_handler(httpd_req_t* req)
{
    if (req->method == HTTP_GET) {
        s_state.client_count++;
        ESP_LOGI(TAG, "ws connect: clients=%d", s_state.client_count);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) return err;

    if (frame.len == 0) return ESP_OK;

    char* buf = (char*)calloc(1, frame.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    frame.payload = (uint8_t*)buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    buf[frame.len] = 0;

    if (strcmp(buf, "STOP") == 0) {
        s_state.throttle = 0;
        s_state.steer = 0;
    } else {
        parse_drive_line(buf, &s_state.throttle, &s_state.steer);
    }

    free(buf);

    // Optional ack
    // ws_send_text(req->handle, httpd_req_to_sockfd(req), "OK");

    return ESP_OK;
}

static esp_err_t start_http_server(httpd_handle_t* out)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;

    httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = http_root_get,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &uri_root);

    httpd_uri_t uri_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = http_ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
    };
    httpd_register_uri_handler(server, &uri_ws);

    *out = server;
    return ESP_OK;
}

/* ---------- Experiment hooks ---------- */

static void ExpWifiRemote_OnEnter(void)
{
    memset(&s_state, 0, sizeof(s_state));

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
        return;
    }

    err = wifi_init_ap("STEM-REMOTE", "12345678");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi ap failed: %s", esp_err_to_name(err));
        return;
    }

    err = start_http_server(&s_state.httpd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http server failed: %s", esp_err_to_name(err));
        return;
    }

    s_state.started = true;
    ESP_LOGI(TAG, "ready: connect phone to SSID STEM-REMOTE, open http://192.168.4.1");
}

static void ExpWifiRemote_OnExit(void)
{
    if (s_state.httpd) {
        httpd_stop(s_state.httpd);
        s_state.httpd = NULL;
    }
    esp_wifi_stop();
    s_state.started = false;
}

static void ExpWifiRemote_OnAction(int action)
{
    // Keep it minimal: allow exit or reset using existing key mapping.
    // Example: if (action == 4) ... (match your system)
    (void)action;
}

static void ExpWifiRemote_Tick(void)
{
    // This is where you should apply s_state.throttle / s_state.steer to your car motor control.
    // Keep it lightweight here.
}

static void ExpWifiRemote_Render(void)
{
    Ui_Clear();

    Ui_Printf(0, 0, "WiFi Remote");
    if (!s_state.started) {
        Ui_Printf(0, 2, "Status: NOT STARTED");
        Ui_Render();
        return;
    }

    Ui_Printf(0, 2, "SSID: STEM-REMOTE");
    Ui_Printf(0, 3, "IP:   192.168.4.1");
    Ui_Printf(0, 4, "WS:   /ws");
    Ui_Printf(0, 6, "Clients: %d", s_state.client_count);
    Ui_Printf(0, 7, "Throttle: %d", s_state.throttle);
    Ui_Printf(0, 8, "Steer:    %d", s_state.steer);

    Ui_Render();
}

/* ---------- Register ---------- */

static Experiment s_exp = {
    .name = "WiFi Remote",
    .on_enter = ExpWifiRemote_OnEnter,
    .on_exit = ExpWifiRemote_OnExit,
    .on_action = ExpWifiRemote_OnAction,
    .tick = ExpWifiRemote_Tick,
    .render = ExpWifiRemote_Render,
};

const Experiment* ExpWifiRemote_Get(void)
{
    return &s_exp;
}
