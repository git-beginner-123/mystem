#include "experiments/experiment.h"
#include "ui/ui.h"
#include "audio/sfx.h"

#include "comm_wifi.h"
#include "sdkconfig.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"
#include "display/st7789.h"
#include "qrcode.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static const char* TAG = "EXP_SYSTEM";
static const char* kSystemTitle = "SETTING";
static const char* kSystemCopyright = "Copyright (C) 2026 SEESAW";
static const char* kOtaGameLatestBase = "https://raw.githubusercontent.com/git-beginner-123/OTA/main/ota_bins/game";
static const char* kOtaStemLatestBin = "https://raw.githubusercontent.com/git-beginner-123/OTA/main/ota_bins/stem/latest.bin";

typedef enum {
    kStateSelectAp = 0,
    kStateEditPass,
    kStateQrProvision,
    kStateConnecting,
    kStateDownloading,
    kStateSuccess,
    kStateFail,
} SystemState;

static TaskHandle_t s_ota_task = NULL;
static volatile bool s_abort = false;
static volatile SystemState s_state = kStateSelectAp;
static volatile int s_progress = 0;
static bool s_ui_dirty = true;
static char s_status[64] = "Select AP";
static bool s_need_full_redraw = true;
static uint32_t s_last_anim_ms = 0;
static int s_anim_phase = 0;

static CommWifiAp s_aps[3];
static int s_ap_count = 0;
static int s_ap_sel = 0;
static char s_sel_ssid[33];
static char s_pass[33];
static int s_pass_len = 0;
static int s_pass_sel = 0;
static bool s_qr_ready = false;
static int s_qr_size = 0;
static uint8_t* s_qr_matrix = NULL;
static size_t s_qr_matrix_cap = 0;
static char s_qr_payload[192];
static char s_ota_url[200];

typedef enum {
    kOtaTargetGo = 0,
    kOtaTargetChess,
    kOtaTargetGomoku,
    kOtaTargetDice,
    kOtaTargetStem,
    kOtaTargetCount,
} OtaTarget;

static OtaTarget s_ota_target = kOtaTargetStem;

static const char kPassChars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789"
    "!@#$%^&*_-.";

#define PASS_TOKEN_DEL  (-1)
#define PASS_TOKEN_OK   (-2)
#define OTA_TASK_STACK_BYTES      6144
#define OTA_QR_TASK_STACK_BYTES   7168

static void ota_task_exit(void)
{
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "task=%s stack_hwm=%u", pcTaskGetName(NULL), (unsigned)hwm);
    s_ota_task = NULL;
    vTaskDelete(NULL);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint16_t c_text(void) { return Ui_ColorRGB(225, 230, 235); }
static uint16_t c_ok(void) { return Ui_ColorRGB(120, 240, 120); }
static uint16_t c_err(void) { return Ui_ColorRGB(255, 130, 130); }
static uint16_t c_info(void) { return Ui_ColorRGB(170, 210, 255); }
static uint16_t c_qr_bg(void) { return Ui_ColorRGB(250, 250, 250); }
static uint16_t c_qr_fg(void) { return Ui_ColorRGB(10, 10, 10); }

static const char* ota_target_name(OtaTarget t)
{
    switch (t) {
    case kOtaTargetGo: return "GO";
    case kOtaTargetChess: return "CHESS";
    case kOtaTargetGomoku: return "GOMOKU";
    case kOtaTargetDice: return "DICE";
    case kOtaTargetStem: return "STEM";
    default: return "STEM";
    }
}

static const char* ota_target_slug(OtaTarget t)
{
    switch (t) {
    case kOtaTargetGo: return "go";
    case kOtaTargetChess: return "chess";
    case kOtaTargetGomoku: return "gomoku";
    case kOtaTargetDice: return "dice";
    case kOtaTargetStem: return "stem";
    default: return "stem";
    }
}

static OtaTarget ota_target_from_url(const char* url)
{
    if (!url) return kOtaTargetStem;
    if (strstr(url, "/ota_bins/game/go/")) return kOtaTargetGo;
    if (strstr(url, "/ota_bins/game/chess/")) return kOtaTargetChess;
    if (strstr(url, "/ota_bins/game/gomoku/")) return kOtaTargetGomoku;
    if (strstr(url, "/ota_bins/game/dice/")) return kOtaTargetDice;
    if (strstr(url, "/ota_bins/stem/")) return kOtaTargetStem;
    return kOtaTargetStem;
}

static bool build_selected_ota_url(char* out, size_t cap)
{
    if (!out || cap < 16) return false;
    out[0] = 0;
    if (s_ota_target == kOtaTargetStem) {
        snprintf(out, cap, "%s", kOtaStemLatestBin);
    } else {
        snprintf(out, cap, "%s/%s/latest.bin", kOtaGameLatestBase, ota_target_slug(s_ota_target));
    }
    return true;
}

static void append_cache_buster(char* url, size_t cap)
{
    if (!url || cap < 8) return;
    if (!strstr(url, "/latest.bin")) return;
    size_t len = strlen(url);
    if (len + 20 >= cap) return;
    char sep = (strchr(url, '?') != NULL) ? '&' : '?';
    uint32_t stamp = now_ms() ^ (esp_random() & 0xFFFFU);
    snprintf(url + len, cap - len, "%ccb=%u", sep, (unsigned)stamp);
}

static uint32_t ssid_hash(const char* s)
{
    uint32_t h = 5381U;
    while (s && *s) {
        h = ((h << 5) + h) ^ (uint8_t)(*s++);
    }
    return h;
}

static bool load_saved_pass(const char* ssid, char* out_pass, size_t cap)
{
    if (!ssid || !ssid[0] || !out_pass || cap < 2) return false;
    nvs_handle_t nvs = 0;
    if (nvs_open("syswifi", NVS_READONLY, &nvs) != ESP_OK) return false;

    char key[16];
    snprintf(key, sizeof(key), "p%08x", (unsigned)ssid_hash(ssid));

    size_t req = cap;
    esp_err_t err = nvs_get_str(nvs, key, out_pass, &req);
    nvs_close(nvs);
    return err == ESP_OK && out_pass[0] != 0;
}

static void save_pass(const char* ssid, const char* pass)
{
    if (!ssid || !ssid[0] || !pass || !pass[0]) return;
    nvs_handle_t nvs = 0;
    if (nvs_open("syswifi", NVS_READWRITE, &nvs) != ESP_OK) return;

    char key[16];
    snprintf(key, sizeof(key), "p%08x", (unsigned)ssid_hash(ssid));
    if (nvs_set_str(nvs, key, pass) == ESP_OK) {
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

static const char* app_version(void)
{
    const esp_app_desc_t* d = esp_app_get_description();
    if (!d || !d->version[0]) return "unknown";
    return d->version;
}

static void set_state(SystemState st, const char* status, int progress)
{
    SystemState prev = s_state;
    bool dynamic_state = (st == kStateConnecting || st == kStateDownloading);
    s_state = st;
    s_progress = progress;
    if (!status) status = "";
    strncpy(s_status, status, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = 0;
    if (prev != st || !dynamic_state) {
        s_need_full_redraw = true;
        s_anim_phase = 0;
    }
    s_ui_dirty = true;
}

static void draw_progress_bar(int pct, bool busy)
{
    // Keep geometry aligned with Ui body area constants in ui_lcd.c.
    const int bar_x = 10;
    const int bar_y = 206;
    const int bar_w = 220;
    const int bar_h = 14;
    const uint16_t bg = Ui_ColorRGB(24, 28, 34);
    const uint16_t fg = Ui_ColorRGB(70, 190, 255);
    const uint16_t frame = Ui_ColorRGB(110, 130, 150);

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    St7789_FillRect(bar_x - 1, bar_y - 1, bar_w + 2, bar_h + 2, frame);
    St7789_FillRect(bar_x, bar_y, bar_w, bar_h, bg);

    if (busy && pct == 0) {
        // Animated waiting segment before byte progress starts.
        int seg_w = 36;
        int max_x = bar_w - seg_w;
        if (max_x < 1) max_x = 1;
        int x = (s_anim_phase * 7) % max_x;
        St7789_FillRect(bar_x + x, bar_y + 2, seg_w, bar_h - 4, fg);
    } else {
        int fill = (bar_w * pct) / 100;
        if (fill > 0) St7789_FillRect(bar_x, bar_y + 2, fill, bar_h - 4, fg);
    }
}

static void draw_status_dynamic_rows(void)
{
    char line[64];
    char status_line[64];

    if (s_state == kStateDownloading) {
        const char spinner[] = "|/-\\";
        int ph = s_anim_phase & 3;
        snprintf(status_line, sizeof(status_line), "Status: %c %.52s", spinner[ph], s_status);
    } else {
        snprintf(status_line, sizeof(status_line), "Status: %.55s", s_status);
    }

    uint16_t status_color = c_text();
    if (s_state == kStateFail) status_color = c_err();
    else if (s_state == kStateSuccess) status_color = c_ok();
    Ui_DrawBodyTextRowColor(3, status_line, status_color);

    if (s_state == kStateDownloading) {
        if (s_progress > 0) snprintf(line, sizeof(line), "Progress: %d%%", s_progress);
        else snprintf(line, sizeof(line), "Progress: preparing...");
        Ui_DrawBodyTextRowColor(4, line, c_text());
        draw_progress_bar(s_progress, true);
    } else if (s_state == kStateConnecting && comm_wifi_is_connected()) {
        Ui_DrawBodyTextRowColor(4, "Press OK to start OTA", c_info());
    } else {
        snprintf(line, sizeof(line), "OTA: %s", ota_target_name(s_ota_target));
        Ui_DrawBodyTextRowColor(4, line, c_text());
    }
}

static int pass_token_count(void)
{
    return (int)strlen(kPassChars) + 2;
}

static int pass_token_from_sel(int sel)
{
    int n = (int)strlen(kPassChars);
    if (sel < n) return (int)kPassChars[sel];
    if (sel == n) return PASS_TOKEN_DEL;
    return PASS_TOKEN_OK;
}

static void rescan_aps(void)
{
    s_ap_count = comm_wifi_scan_top3(s_aps, 3);
    if (s_ap_count <= 0) {
        s_ap_count = 0;
        s_ap_sel = 0;
        set_state(kStateSelectAp, "No AP found. OK=rescan", 0);
    } else {
        if (s_ap_sel >= s_ap_count) s_ap_sel = 0;
        set_state(kStateSelectAp, "Select AP then OK", 0);
    }
}

static bool perform_ota_http(const char* url, char* errbuf, size_t errcap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = CONFIG_COMM_WIFI_OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };

    if (strncmp(url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(errbuf, errcap, "http init failed");
        return false;
    }

    const esp_partition_t* update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        snprintf(errbuf, errcap, "no OTA partition");
        esp_http_client_cleanup(client);
        return false;
    }

    esp_ota_handle_t ota_handle = 0;
    bool opened = false;
    bool begun = false;
    bool ok = false;
    int content_len = -1;
    int downloaded = 0;
    int last_report_bucket = -1;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        snprintf(errbuf, errcap, "http open: %s", esp_err_to_name(err));
        goto cleanup;
    }
    opened = true;

    content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        snprintf(errbuf, errcap, "http status %d", status);
        goto cleanup;
    }

    err = esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        snprintf(errbuf, errcap, "ota begin: %s", esp_err_to_name(err));
        goto cleanup;
    }
    begun = true;

    uint8_t buf[1024];
    while (!s_abort) {
        int n = esp_http_client_read(client, (char*)buf, sizeof(buf));
        if (n < 0) {
            snprintf(errbuf, errcap, "http read fail");
            goto cleanup;
        }
        if (n == 0) break;

        err = esp_ota_write(ota_handle, buf, (size_t)n);
        if (err != ESP_OK) {
            snprintf(errbuf, errcap, "ota write: %s", esp_err_to_name(err));
            goto cleanup;
        }

        downloaded += n;
        int progress = 0;
        if (content_len > 0) {
            progress = (downloaded * 100) / content_len;
            if (progress > 100) progress = 100;
        }

        int report_bucket = downloaded / 8192;
        if (report_bucket != last_report_bucket) {
            char line[64];
            if (content_len > 0) {
                snprintf(line, sizeof(line), "Downloading... %d%%", progress);
            } else {
                snprintf(line, sizeof(line), "Downloading... %dKB", downloaded / 1024);
            }
            set_state(kStateDownloading, line, progress);
            last_report_bucket = report_bucket;
        }
    }

    if (s_abort) {
        snprintf(errbuf, errcap, "aborted");
        goto cleanup;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        snprintf(errbuf, errcap, "ota end: %s", esp_err_to_name(err));
        goto cleanup;
    }
    begun = false;

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        snprintf(errbuf, errcap, "set boot: %s", esp_err_to_name(err));
        goto cleanup;
    }

    ok = true;

cleanup:
    if (begun) esp_ota_abort(ota_handle);
    if (opened) esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

static void qr_capture_cb(const uint8_t* qrcode)
{
    int sz = esp_qrcode_get_size(qrcode);
    if (sz <= 0 || sz > 177) {
        s_qr_ready = false;
        return;
    }
    size_t need = (size_t)sz * (size_t)sz;
    if (!s_qr_matrix || s_qr_matrix_cap < need) {
        uint8_t* p = (uint8_t*)realloc(s_qr_matrix, need);
        if (!p) {
            s_qr_ready = false;
            return;
        }
        s_qr_matrix = p;
        s_qr_matrix_cap = need;
    }
    s_qr_size = sz;
    for (int y = 0; y < sz; y++) {
        for (int x = 0; x < sz; x++) {
            s_qr_matrix[y * sz + x] = esp_qrcode_get_module(qrcode, x, y) ? 1 : 0;
        }
    }
    s_qr_ready = true;
}

static void build_qr_payload(const char* service_name, const char* pop)
{
    if (pop && pop[0]) {
        snprintf(s_qr_payload, sizeof(s_qr_payload),
                 "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"softap\"}",
                 service_name, pop);
    } else {
        snprintf(s_qr_payload, sizeof(s_qr_payload),
                 "{\"ver\":\"v1\",\"name\":\"%s\",\"transport\":\"softap\"}",
                 service_name);
    }
}

static bool gen_qr_for_lcd(const char* service_name, const char* pop)
{
    s_qr_ready = false;
    s_qr_size = 0;
    build_qr_payload(service_name, pop);

    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = qr_capture_cb;
    cfg.max_qrcode_version = 12;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
    esp_err_t err = esp_qrcode_generate(&cfg, s_qr_payload);
    if (err != ESP_OK || !s_qr_ready) {
        ESP_LOGW(TAG, "QR gen failed err=%s ready=%d payload=%s",
                 esp_err_to_name(err), (int)s_qr_ready, s_qr_payload);
        return false;
    }
    ESP_LOGI(TAG, "QR ready size=%d", s_qr_size);
    return true;
}

static void free_qr_matrix(void)
{
    if (s_qr_matrix) {
        free(s_qr_matrix);
        s_qr_matrix = NULL;
    }
    s_qr_matrix_cap = 0;
}

static void draw_qr_on_lcd(void)
{
    const int area_x = 20;
    const int area_y = 110;
    const int area_w = 200;
    const int area_h = 176;

    if (!s_qr_ready || s_qr_size <= 0) {
        St7789_FillRect(area_x, area_y, area_w, area_h, c_qr_bg());
        St7789_FillRect(area_x + 8, area_y + 8, area_w - 16, area_h - 16, c_qr_fg());
        St7789_FillRect(area_x + 12, area_y + 12, area_w - 24, area_h - 24, c_qr_bg());
        Ui_DrawTextAtBg(area_x + 66, area_y + 72, "NO QR", c_err(), c_qr_bg());
        return;
    }

    const int quiet = 2;
    int full = s_qr_size + quiet * 2;
    int scale = area_w / full;
    if (area_h / full < scale) scale = area_h / full;
    if (scale < 1) scale = 1;

    int draw_w = full * scale;
    int draw_h = full * scale;
    int ox = area_x + (area_w - draw_w) / 2;
    int oy = area_y + (area_h - draw_h) / 2;

    St7789_FillRect(area_x, area_y, area_w, area_h, c_qr_bg());

    for (int y = 0; y < s_qr_size; y++) {
        for (int x = 0; x < s_qr_size; x++) {
            if (!s_qr_matrix[y * s_qr_size + x]) continue;
            int px = ox + (x + quiet) * scale;
            int py = oy + (y + quiet) * scale;
            St7789_FillRect(px, py, scale, scale, c_qr_fg());
        }
    }
}

static bool run_qr_provision(char* errbuf, size_t errcap)
{
    const char* service_name = "SYSTEM_PROV";
    const char* pop = "SEESAW2026";

    wifi_prov_mgr_config_t cfg = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE
    };

    esp_err_t err = wifi_prov_mgr_init(cfg);
    if (err != ESP_OK) {
        snprintf(errbuf, errcap, "prov init: %s", esp_err_to_name(err));
        return false;
    }

    bool provisioned = false;
    err = wifi_prov_mgr_is_provisioned(&provisioned);
    if (err != ESP_OK) {
        snprintf(errbuf, errcap, "prov state: %s", esp_err_to_name(err));
        wifi_prov_mgr_deinit();
        return false;
    }

    if (!provisioned) {
        set_state(kStateQrProvision, "Scan QR with ESP SoftAP app", 0);
        if (!gen_qr_for_lcd(service_name, pop)) {
            set_state(kStateFail, "QR generate failed", 0);
            wifi_prov_mgr_deinit();
            return false;
        }
        err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, pop, service_name, NULL);
        if (err != ESP_OK) {
            snprintf(errbuf, errcap, "prov start: %s", esp_err_to_name(err));
            wifi_prov_mgr_deinit();
            return false;
        }
    } else {
        set_state(kStateQrProvision, "Already provisioned, connecting", 0);
    }

    comm_wifi_start();
    uint32_t t0 = now_ms();
    while (!s_abort && !comm_wifi_is_connected()) {
        if ((now_ms() - t0) > 120000U) {
            snprintf(errbuf, errcap, "provision timeout");
            wifi_prov_mgr_stop_provisioning();
            wifi_prov_mgr_deinit();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    wifi_prov_mgr_stop_provisioning();
    wifi_prov_mgr_deinit();
    return !s_abort;
}

static void ota_task(void* arg)
{
    (void)arg;
    char err[64] = {0};
    char ssid[33];
    char pass[33];

    strncpy(ssid, s_sel_ssid, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = 0;
    strncpy(pass, s_pass, sizeof(pass) - 1);
    pass[sizeof(pass) - 1] = 0;

    if (!ssid[0]) {
        set_state(kStateFail, "No AP selected", 0);
        ota_task_exit();
        return;
    }

    if (!build_selected_ota_url(s_ota_url, sizeof(s_ota_url))) {
        set_state(kStateFail, "Set valid OTA URL", 0);
        ota_task_exit();
        return;
    }

    set_state(kStateConnecting, "Connecting WiFi...", 0);
    comm_wifi_start();
    if (!comm_wifi_connect_psk(ssid, pass)) {
        set_state(kStateFail, "WiFi connect start failed", 0);
        ota_task_exit();
        return;
    }

    uint32_t t0 = now_ms();
    while (!s_abort && !comm_wifi_is_connected()) {
        if ((now_ms() - t0) > (uint32_t)CONFIG_COMM_WIFI_CONNECT_TIMEOUT_MS) {
            set_state(kStateFail, "WiFi connect timeout", 0);
            ota_task_exit();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (s_abort) {
        set_state(kStateFail, "Upgrade canceled", 0);
        ota_task_exit();
        return;
    }

    // Cache password for this SSID after connection succeeds.
    save_pass(ssid, pass);

    set_state(kStateDownloading, "Downloading... 0%", 0);
    ESP_LOGI(TAG, "OTA URL: %s", s_ota_url);
    if (!perform_ota_http(s_ota_url, err, sizeof(err))) {
        set_state(kStateFail, err[0] ? err : "OTA failed", 0);
        ota_task_exit();
        return;
    }

    Sfx_PlayVictory();
    set_state(kStateSuccess, "Upgrade success. Rebooting...", 100);
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static void qr_ota_task(void* arg)
{
    (void)arg;
    char err[64] = {0};

    if (!build_selected_ota_url(s_ota_url, sizeof(s_ota_url))) {
        set_state(kStateFail, "Set valid OTA URL", 0);
        ota_task_exit();
        return;
    }

    if (!run_qr_provision(err, sizeof(err))) {
        set_state(kStateFail, err[0] ? err : "QR provision failed", 0);
        ota_task_exit();
        return;
    }

    set_state(kStateDownloading, "Downloading... 0%", 0);
    ESP_LOGI(TAG, "OTA URL: %s", s_ota_url);
    if (!perform_ota_http(s_ota_url, err, sizeof(err))) {
        set_state(kStateFail, err[0] ? err : "OTA failed", 0);
        ota_task_exit();
        return;
    }

    Sfx_PlayVictory();
    set_state(kStateSuccess, "Upgrade success. Rebooting...", 100);
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static void ota_task_connected(void* arg)
{
    (void)arg;
    char err[64] = {0};

    if (!build_selected_ota_url(s_ota_url, sizeof(s_ota_url))) {
        set_state(kStateFail, "Set valid OTA URL", 0);
        ota_task_exit();
        return;
    }
    append_cache_buster(s_ota_url, sizeof(s_ota_url));

    if (!comm_wifi_is_connected()) {
        set_state(kStateFail, "WiFi disconnected", 0);
        ota_task_exit();
        return;
    }

    set_state(kStateDownloading, "Downloading... 0%", 0);
    ESP_LOGI(TAG, "OTA URL: %s", s_ota_url);
    if (!perform_ota_http(s_ota_url, err, sizeof(err))) {
        set_state(kStateFail, err[0] ? err : "OTA failed", 0);
        ota_task_exit();
        return;
    }

    Sfx_PlayVictory();
    set_state(kStateSuccess, "Upgrade success. Rebooting...", 100);
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static void draw_ui(void)
{
    char line[64];
    char masked[33] = {0};

    if (s_need_full_redraw) {
        Ui_DrawFrame(kSystemTitle, "UP/DN:SEL LR:TGT OK:SET");
        Ui_DrawBodyClear();
    }

    if (s_need_full_redraw) {
        // Shorter label leaves more room for git-describe suffix.
        snprintf(line, sizeof(line), "VER: %.58s", app_version());
        Ui_DrawBodyTextRowColor(0, line, c_info());
        Ui_DrawBodyTextRowColor(1, kSystemCopyright, c_info());
    }

    if (s_state == kStateSelectAp) {
        Ui_DrawBodyTextRowColor(2, "AP List:", c_text());
        if (s_ap_count <= 0) {
            Ui_DrawBodyTextRowColor(3, "No AP found", c_err());
            Ui_DrawBodyTextRowColor(4, "Press OK to rescan", c_text());
        } else {
            for (int i = 0; i < s_ap_count && i < 3; i++) {
                char ap_line[64];
                snprintf(ap_line, sizeof(ap_line), "%c %s (%d)",
                         (i == s_ap_sel) ? '>' : ' ', s_aps[i].ssid, s_aps[i].rssi);
                Ui_DrawBodyTextRowColor(3 + i, ap_line, c_text());
            }
            snprintf(line, sizeof(line), "TARGET: %s", ota_target_name(s_ota_target));
            Ui_DrawBodyTextRowColor(6, line, c_ok());
            Ui_DrawBodyTextRowColor(7, "LR: GO/CHESS/DICE/GOMOKU/STEM", c_info());
        }
        return;
    }

    if (s_state == kStateEditPass) {
        snprintf(line, sizeof(line), "SSID: %s", s_sel_ssid);
        Ui_DrawBodyTextRowColor(2, line, c_text());

        for (int i = 0; i < s_pass_len && i < (int)sizeof(masked) - 1; i++) masked[i] = '*';
        masked[s_pass_len] = 0;
        snprintf(line, sizeof(line), "PASS[%d]: %s", s_pass_len, masked[0] ? masked : "(empty)");
        Ui_DrawBodyTextRowColor(3, line, c_text());

        int tok = pass_token_from_sel(s_pass_sel);
        if (tok == PASS_TOKEN_DEL) snprintf(line, sizeof(line), "SEL: <DEL>");
        else if (tok == PASS_TOKEN_OK) snprintf(line, sizeof(line), "SEL: <CONNECT>");
        else snprintf(line, sizeof(line), "SEL: '%c'", (char)tok);
        Ui_DrawBodyTextRowColor(4, line, c_text());
        Ui_DrawBodyTextRowColor(5, "OK:add/del/connect", c_text());
        snprintf(line, sizeof(line), "TARGET: %s", ota_target_name(s_ota_target));
        Ui_DrawBodyTextRowColor(6, line, c_ok());
        Ui_DrawBodyTextRowColor(7, "LR: GO/CHESS/DICE/GOMOKU/STEM", c_info());
        return;
    }

    if (s_state == kStateQrProvision) {
        Ui_DrawBodyTextRowColor(2, "QR Provisioning...", c_text());
        Ui_DrawBodyTextRowColor(3, "Scan this code with app", c_info());
        Ui_DrawBodyTextRowColor(4, "APP: Espressif SoftAP", c_info());
        draw_qr_on_lcd();
        return;
    }

    if (s_need_full_redraw) {
        snprintf(line, sizeof(line), "SSID: %s", s_sel_ssid[0] ? s_sel_ssid : "(none)");
        Ui_DrawBodyTextRowColor(2, line, c_text());
        snprintf(line, sizeof(line), "TARGET: %s", ota_target_name(s_ota_target));
        Ui_DrawBodyTextRowColor(6, line, c_ok());
        Ui_DrawBodyTextRowColor(7, "LR: GO/CHESS/DICE/GOMOKU/STEM", c_info());
    }

    draw_status_dynamic_rows();

    if (s_state == kStateConnecting && comm_wifi_is_connected()) {
        Ui_DrawBodyTextRowColor(5, "OK: start OTA  DN: forget WiFi", c_text());
    }

    if (s_state == kStateFail) {
        Ui_DrawBodyTextRowColor(5, "OK: rescan AP", c_text());
    }

    s_need_full_redraw = false;
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame(kSystemTitle, "OK:START  BACK");
    Ui_Println("1) SYSTEM OTA");
    Ui_Println("2) Select AP + password");
    Ui_Println("3) LR choose OTA app");
    Ui_Println("4) Download and upgrade");
    Ui_Println("Success: auto reboot");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    s_abort = false;
    s_progress = 0;
    s_ap_count = 0;
    s_ap_sel = 0;
    s_sel_ssid[0] = 0;
    s_pass[0] = 0;
    s_pass_len = 0;
    s_pass_sel = 0;
    s_qr_ready = false;
    s_qr_size = 0;
    s_qr_payload[0] = 0;
    s_ota_url[0] = 0;
    s_ota_target = ota_target_from_url(CONFIG_COMM_WIFI_OTA_URL);
    (void)build_selected_ota_url(s_ota_url, sizeof(s_ota_url));
    strncpy(s_status, "Select AP", sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = 0;
    s_need_full_redraw = true;
    s_last_anim_ms = 0;
    s_anim_phase = 0;

    comm_wifi_start();
    if (comm_wifi_is_connected()) {
        if (!comm_wifi_get_connected_ssid(s_sel_ssid, (int)sizeof(s_sel_ssid))) {
            s_sel_ssid[0] = 0;
        }
        set_state(kStateConnecting, "WiFi ready. OK to start OTA", 0);
    } else {
        rescan_aps();
    }
    s_ui_dirty = true;
    draw_ui();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    s_abort = true;
    free_qr_matrix();
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (s_ota_task) return;

    if (key == kInputLeft || key == kInputRight || key == kInputWhiteLeft || key == kInputWhiteRight) {
        if (key == kInputLeft || key == kInputWhiteLeft) {
            s_ota_target = (s_ota_target == 0) ? (kOtaTargetCount - 1) : (s_ota_target - 1);
        } else {
            s_ota_target = (s_ota_target + 1) % kOtaTargetCount;
        }
        (void)build_selected_ota_url(s_ota_url, sizeof(s_ota_url));
        snprintf(s_status, sizeof(s_status), "Target: %s", ota_target_name(s_ota_target));
        s_need_full_redraw = true;
        s_ui_dirty = true;
        return;
    }

    if (s_state == kStateConnecting && comm_wifi_is_connected() && key == kInputEnter) {
        s_abort = false;
        if (xTaskCreate(ota_task_connected, "ota_task_conn", OTA_TASK_STACK_BYTES, NULL, 4, &s_ota_task) != pdPASS) {
            set_state(kStateFail, "OTA task create failed", 0);
        }
        return;
    }

    if (s_state == kStateConnecting && comm_wifi_is_connected() && key == kInputDown) {
        if (comm_wifi_forget_saved_and_disconnect()) {
            s_sel_ssid[0] = 0;
            s_pass[0] = 0;
            s_pass_len = 0;
            set_state(kStateSelectAp, "WiFi forgotten. Reconnect required", 0);
            rescan_aps();
        } else {
            set_state(kStateFail, "Forget WiFi failed", 0);
        }
        return;
    }

    if (s_state == kStateSelectAp) {
        if (key == kInputUp && s_ap_count > 0) {
            s_ap_sel--;
            if (s_ap_sel < 0) s_ap_sel = s_ap_count - 1;
            s_ui_dirty = true;
            return;
        }
        if (key == kInputDown && s_ap_count > 0) {
            s_ap_sel++;
            if (s_ap_sel >= s_ap_count) s_ap_sel = 0;
            s_ui_dirty = true;
            return;
        }
        if (key == kInputEnter) {
            if (s_ap_count <= 0) {
                rescan_aps();
                return;
            }
            strncpy(s_sel_ssid, s_aps[s_ap_sel].ssid, sizeof(s_sel_ssid) - 1);
            s_sel_ssid[sizeof(s_sel_ssid) - 1] = 0;
            if (load_saved_pass(s_sel_ssid, s_pass, sizeof(s_pass))) {
                s_pass_len = (int)strlen(s_pass);
                s_abort = false;
                set_state(kStateConnecting, "Using saved password...", 0);
                if (xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK_BYTES, NULL, 4, &s_ota_task) != pdPASS) {
                    set_state(kStateFail, "OTA task create failed", 0);
                }
            } else {
                s_abort = false;
                set_state(kStateQrProvision, "Starting QR provision...", 0);
                if (xTaskCreate(qr_ota_task, "qr_ota_task", OTA_QR_TASK_STACK_BYTES, NULL, 4, &s_ota_task) != pdPASS) {
                    set_state(kStateFail, "QR task create failed", 0);
                }
            }
            return;
        }
        return;
    }

    if (s_state == kStateEditPass) {
        int n = pass_token_count();
        if (key == kInputUp) {
            s_pass_sel--;
            if (s_pass_sel < 0) s_pass_sel = n - 1;
            s_ui_dirty = true;
            return;
        }
        if (key == kInputDown) {
            s_pass_sel++;
            if (s_pass_sel >= n) s_pass_sel = 0;
            s_ui_dirty = true;
            return;
        }
        if (key == kInputEnter) {
            int tok = pass_token_from_sel(s_pass_sel);
            if (tok == PASS_TOKEN_DEL) {
                if (s_pass_len > 0) {
                    s_pass_len--;
                    s_pass[s_pass_len] = 0;
                }
                s_ui_dirty = true;
                return;
            }
            if (tok == PASS_TOKEN_OK) {
                s_abort = false;
                set_state(kStateConnecting, "Connecting WiFi...", 0);
                if (xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK_BYTES, NULL, 4, &s_ota_task) != pdPASS) {
                    set_state(kStateFail, "OTA task create failed", 0);
                }
                return;
            }
            if (s_pass_len < (int)sizeof(s_pass) - 1) {
                s_pass[s_pass_len++] = (char)tok;
                s_pass[s_pass_len] = 0;
            }
            s_ui_dirty = true;
            return;
        }
        return;
    }

    if (s_state == kStateFail && key == kInputEnter) {
        rescan_aps();
    }
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
    uint32_t t = now_ms();
    if (s_state == kStateDownloading) {
        if ((t - s_last_anim_ms) >= 120U) {
            s_last_anim_ms = t;
            s_anim_phase++;
            s_ui_dirty = true;
        }
    }
    if (!s_ui_dirty) return;
    draw_ui();
    s_ui_dirty = false;
}

const Experiment g_exp_wifi_ota = {
    .id = 16,
    .title = "SYSTEM",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
