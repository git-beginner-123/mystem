#include "experiments/experiment.h"
#include "ui/ui.h"

#include <string.h>
#include <stdio.h>

#include "esp_timer.h"
#include "net/market_yahoo.h"
#include "comm_wifi.h"

typedef enum {
    kStageSelectAp = 0,
    kStageConnecting,
    kStageConnected,
    kStageNoAp
} WifiStaStage;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static const char* kSymbols[] = {
    "GC=F", // Gold futures
    "SI=F", // Silver futures
    "BZ=F", // Brent crude oil futures
};

static uint32_t s_next_ui_ms = 0;
static uint32_t s_next_scan_ms = 0;
static WifiStaStage s_stage = kStageSelectAp;
static CommWifiAp s_aps[3];
static int s_ap_count = 0;
static int s_ap_sel = 0;
static const char* kWifiPass = "Abcdefg_123!";
static char s_sel_ssid[33];

static char s_line_cache[3][48];
static bool s_connected_screen = false;
static bool s_scan_screen = false;

static uint16_t color_gold(void)   { return Ui_ColorRGB(212, 175, 55); }
static uint16_t color_silver(void) { return Ui_ColorRGB(192, 192, 192); }
static uint16_t color_oil(void)    { return Ui_ColorRGB(139,  69,  19); }
static uint16_t color_value(void)  { return Ui_ColorRGB(230, 230, 230); }

static void render_ap_list(void)
{
    Ui_DrawFrame("WIFI STA", "BACK=RET");
    Ui_DrawBodyClear();
    Ui_DrawBodyTextRowColor(0, "STATUS: SCAN", color_value());

    if (s_ap_count <= 0) {
        Ui_DrawBodyTextRowColor(1, "No AP found", color_value());
        Ui_DrawBodyTextRowColor(2, "ENTER=Rescan", color_value());
        Ui_DrawBodyTextRowColor(3, "", color_value());
        return;
    }

    for (int i = 0; i < s_ap_count; i++) {
        char line[48];
        const char* mark = (i == s_ap_sel) ? ">" : " ";
        snprintf(line, sizeof(line), "%s %s (%d)", mark, s_aps[i].ssid, s_aps[i].rssi);
        Ui_DrawBodyTextRowColor(i + 1, line, color_value());
    }
}

static void scan_and_show(void)
{
    s_ap_count = comm_wifi_scan_top3(s_aps, 3);
    if (s_ap_count <= 0) {
        s_stage = kStageNoAp;
    } else {
        s_stage = kStageSelectAp;
        if (s_ap_sel >= s_ap_count) s_ap_sel = 0;
    }
    render_ap_list();
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_Clear();
    Ui_Println("WIFI STA");
    Ui_Println("GOAL: fetch markets");
    Ui_Println("SRC:  Yahoo chart");
    Ui_Println("PER:  30s/one symbol");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    comm_wifi_start();
    Ui_Clear();
    Ui_Println("WIFI STA");
    Ui_Println("Preparing...");
    Markets_Init(kSymbols, (int)(sizeof(kSymbols) / sizeof(kSymbols[0])));
    s_next_ui_ms = 0;
    s_next_scan_ms = 0;
    s_ap_sel = 0;
    s_sel_ssid[0] = 0;
    memset(s_line_cache, 0, sizeof(s_line_cache));
    s_connected_screen = false;
    s_scan_screen = false;
    scan_and_show();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_Clear();
    Ui_Println("WIFI STA");
    Ui_Println("Stopped");
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;

    if (s_stage == kStageConnected) return;

    if (key == kInputUp && s_ap_count > 0) {
        s_ap_sel--;
        if (s_ap_sel < 0) s_ap_sel = s_ap_count - 1;
        render_ap_list();
        return;
    }

    if (key == kInputDown && s_ap_count > 0) {
        s_ap_sel++;
        if (s_ap_sel >= s_ap_count) s_ap_sel = 0;
        render_ap_list();
        return;
    }

    if (key == kInputEnter) {
        if (s_ap_count <= 0) {
            scan_and_show();
            return;
        }
        Ui_Clear();
        Ui_DrawFrame("WIFI STA", "BACK=RET");
        Ui_DrawBodyClear();
        Ui_DrawBodyTextRowColor(0, "STATUS: CONNECTING", color_value());
        Ui_DrawBodyTextRowColor(1, s_aps[s_ap_sel].ssid, color_value());
        s_stage = kStageConnecting;
        strncpy(s_sel_ssid, s_aps[s_ap_sel].ssid, sizeof(s_sel_ssid) - 1);
        s_sel_ssid[sizeof(s_sel_ssid) - 1] = 0;
        comm_wifi_connect_psk(s_aps[s_ap_sel].ssid, kWifiPass);
        return;
    }
}

static void render_connected_screen_once(void)
{
    if (s_connected_screen) return;
    s_connected_screen = true;

    Ui_DrawFrame("WIFI STA", "BACK=RET");
    Ui_DrawBodyClear();
    if (s_sel_ssid[0]) {
        char line[48];
        snprintf(line, sizeof(line), "STATUS: %s", s_sel_ssid);
        Ui_DrawBodyTextRowColor(0, line, color_value());
    } else {
        Ui_DrawBodyTextRowColor(0, "STATUS: CONNECTED", color_value());
    }
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;

    uint32_t t = now_ms();

    if (s_stage != kStageConnected) {
        if (s_stage == kStageSelectAp || s_stage == kStageNoAp) {
            if (t >= s_next_scan_ms) {
                s_next_scan_ms = t + 20000;
                scan_and_show();
            }
        }
        if (comm_wifi_is_connected()) {
            s_stage = kStageConnected;
            s_connected_screen = false;
            s_scan_screen = false;
            render_connected_screen_once();
            s_next_ui_ms = 0;
        }
        return;
    }

    // Network fetch (all symbols every 30s)
    Markets_Tick(t);

    // UI refresh every 1s, redraw only changed rows
    if (t < s_next_ui_ms) return;
    s_next_ui_ms = t + 1000;

    render_connected_screen_once();

    for (int i = 0; i < Markets_Count() && i < 3; i++) {
        MarketQuote q;
        char value[32];
        const char* label = "";
        uint16_t label_color = color_value();

        if (i == 0) { label = "GOLD";  label_color = color_gold(); }
        if (i == 1) { label = "SILV";  label_color = color_silver(); }
        if (i == 2) { label = "BREN";  label_color = color_oil(); }

        if (Markets_Get(i, &q)) {
            snprintf(value, sizeof(value), "%.1f %+.1fpct", q.price, q.change_pct);
        } else {
            snprintf(value, sizeof(value), "LOADING...");
        }

        if (strcmp(s_line_cache[i], value) != 0) {
            strncpy(s_line_cache[i], value, sizeof(s_line_cache[i]) - 1);
            s_line_cache[i][sizeof(s_line_cache[i]) - 1] = 0;
            Ui_DrawBodyTextRowTwoColor(i + 1, label, value, label_color, color_value());
        }
    }
}

const Experiment g_exp_wifi_sta = {
    .id = 10,
    .title = "WIFI STA",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
