#include "experiments/experiment.h"
#include "ui/ui.h"

#include "comm_ble.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static uint32_t s_last_draw_ms = 0;
static char s_last_screen[256];   // keep only ONE
static bool s_ui_dirty = true;
static CommBleState s_last_state = (CommBleState)(-1);
// -------------------- console (ring of lines) --------------------

#define BLE_LOG_MAX_LINES  64
#define BLE_LOG_LINE_CAP   64

static char s_log[BLE_LOG_MAX_LINES][BLE_LOG_LINE_CAP];
static int  s_log_head = 0;     // next write index
static int  s_log_count = 0;    // valid lines
static int  s_log_first = 0;    // first visible line (0=oldest)
static bool s_log_follow = true;

static void draw_requirements(void)
{
    Ui_DrawFrame("BLE", "OK: START  BACK");

    Ui_Println("GOAL: Write -> +1 notify");
    Ui_Println("NOTE: BLE enabled only in RUN");
}
static void ble_log_clear(void)
{
    memset(s_log, 0, sizeof(s_log));
    s_log_head = 0;
    s_log_count = 0;
    s_log_first = 0;
    s_log_follow = true;
}

static int ble_log_oldest_index(void)
{
    int oldest = s_log_head - s_log_count;
    while (oldest < 0) oldest += BLE_LOG_MAX_LINES;
    return oldest;
}

static int ble_log_ring_index_from_oldest(int index_from_oldest)
{
    int idx = ble_log_oldest_index() + index_from_oldest;
    idx %= BLE_LOG_MAX_LINES;
    return idx;
}

static const char* ble_log_get_line(int index_from_oldest)
{
    if (index_from_oldest < 0 || index_from_oldest >= s_log_count) return "";
    return s_log[ble_log_ring_index_from_oldest(index_from_oldest)];
}

static void ble_log_push_line(const char* s)
{
    if (!s) s = "";

    strncpy(s_log[s_log_head], s, BLE_LOG_LINE_CAP - 1);
    s_log[s_log_head][BLE_LOG_LINE_CAP - 1] = 0;

    s_log_head = (s_log_head + 1) % BLE_LOG_MAX_LINES;
    if (s_log_count < BLE_LOG_MAX_LINES) s_log_count++;

    if (s_log_follow) {
        // keep pinned (first will be adjusted on draw based on visible rows)
        if (s_log_count > 0) s_log_first = s_log_count - 1;
        else s_log_first = 0;
    }
}

// simple wrap (space preferred) into multiple lines
static const char* wrap_next(const char* p, int cols, char* out, int cap)
{
    if (!p) p = "";
    if (cols < 1) cols = 1;
    if (cap < 2) { if (cap == 1) out[0] = 0; return p; }

    while (*p == ' ' || *p == '\t') p++;

    if (*p == '\n') {
        out[0] = 0;
        return p + 1;
    }

    int maxc = cols;
    if (maxc > cap - 1) maxc = cap - 1;

    int n = 0;
    int last_sp_out = -1;
    const char* last_sp_p = NULL;

    while (*p && *p != '\n' && n < maxc) {
        char ch = *p;
        out[n++] = ch;
        if (ch == ' ' || ch == '\t') {
            last_sp_out = n - 1;
            last_sp_p = p;
        }
        p++;
    }

    if (*p == '\n') {
        out[n] = 0;
        return p + 1;
    }

    if (n == maxc && *p && *p != '\n' && last_sp_out >= 0) {
        int cut = last_sp_out;
        while (cut > 0 && (out[cut] == ' ' || out[cut] == '\t')) cut--;
        out[cut + 1] = 0;

        p = last_sp_p + 1;
        while (*p == ' ' || *p == '\t') p++;
        return p;
    }

    out[n] = 0;
    return p;
}

static void ble_log_append_wrapped(const char* text, int cols)
{
    if (!text) text = "";
    const char* p = text;
    while (*p) {
        char line[BLE_LOG_LINE_CAP];
        p = wrap_next(p, cols, line, (int)sizeof(line));
        ble_log_push_line(line);
    }
}

// -------------------- helpers --------------------

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static const char* state_str(CommBleState st)
{
    switch (st) {
    case kCommBleOff:         return "OFF";
    case kCommBleIdle:        return "IDLE";
    case kCommBleAdvertising: return "ADV";
    case kCommBleConnected:   return "CONN";
    default:                  return "UNK";
    }
}

// -------------------- UI draw --------------------


static void draw_run_screen(const char* name, const char* addr, const char* st_short)
{
    Ui_Clear();

    
    Ui_DrawFrame("BLE", "DN: OLDER  OK: NEWER  BACK");
    Ui_Println("BLE RUN");
    char line[64];
    snprintf(line, sizeof(line), "Name: %s", name ? name : "?"); Ui_Println(line);
    snprintf(line, sizeof(line), "Addr: %s", addr ? addr : "?"); Ui_Println(line);
    snprintf(line, sizeof(line), "Stat: %s", st_short ? st_short : "?"); Ui_Println(line);

    Ui_Println("---- LOG ----");

    // Keep this consistent with your previous approach
    int rows_for_log = 3;

    int count = s_log_count;
    int max_first = count - rows_for_log;
    if (max_first < 0) max_first = 0;

    int first = s_log_follow ? max_first : s_log_first;
    if (first < 0) first = 0;
    if (first > max_first) first = max_first;

    for (int i = 0; i < rows_for_log; i++) {
        int idx = first + i;
        const char* s = (idx < count) ? ble_log_get_line(idx) : "";
        Ui_Println(s);
    }    
}

// -------------------- experiment hooks --------------------

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    draw_requirements();
}

static void ble_on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    CommBle_InitOnce();
}

static void ble_on_exit(ExperimentContext* ctx)
{
    (void)ctx;
    CommBle_Enable(false);
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;

    CommBle_ClearLastRx();
    CommBle_Enable(true);

    s_last_screen[0] = 0;
    s_last_state = (CommBleState)(-1);

    ble_log_clear();
    ble_log_push_line("RUN START");

    Ui_Clear();          // clear once
    s_ui_dirty = true;   // force first draw
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    CommBle_Enable(false);
    CommBle_ClearLastRx();

    s_last_screen[0] = 0;
    ble_log_clear();
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;

    if (key == kInputDown) {
        int rows_for_log = 3;
        int max_first = s_log_count - rows_for_log;
        if (max_first < 0) max_first = 0;

        int first = s_log_follow ? max_first : s_log_first;
        first -= 1;
        if (first < 0) first = 0;

        s_log_first = first;
        s_log_follow = (s_log_first >= max_first);

        s_last_draw_ms = 0;
    } else if (key == kInputEnter) {
        int rows_for_log = 3;
        int max_first = s_log_count - rows_for_log;
        if (max_first < 0) max_first = 0;

        int first = s_log_follow ? max_first : s_log_first;
        first += 1;
        if (first > max_first) first = max_first;

        s_log_first = first;
        s_log_follow = (s_log_first >= max_first);

        s_last_draw_ms = 0;
    }
    s_ui_dirty = true;
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;

    uint32_t t = now_ms();
    if (t - s_last_draw_ms < 150) return;   // faster check is OK; real draw is gated by dirty

    char addr[24];
    CommBle_GetAddrStr(addr, sizeof(addr));

    CommBleState st = CommBle_GetState();
    const char* st_s = state_str(st);

    // Detect state change -> dirty + log once
    if (st != s_last_state) {
        s_last_state = st;
        s_ui_dirty = true;

        char line[32];
        snprintf(line, sizeof(line), "STATE: %s", st_s);
        ble_log_push_line(line);
    }

    // RX as hex
    uint8_t buf[16];
    int len = CommBle_GetLastRx(buf, (int)sizeof(buf));

    char rxline[96];
    rxline[0] = 0;

    if (len > 0) {
        for (int i = 0; i < len; i++) {
            char tmp[4];
            snprintf(tmp, sizeof(tmp), "%02X ", buf[i]);
            strncat(rxline, tmp, sizeof(rxline) - strlen(rxline) - 1);
        }
    }

    // Signature to detect RX/content change
    char screen[256];
    snprintf(screen, sizeof(screen),
             "N:%s|A:%s|S:%s|R:%s",
             CommBle_GetName(), addr, st_s, (len > 0) ? rxline : "(none)");

    if (strcmp(screen, s_last_screen) != 0) {
        strncpy(s_last_screen, screen, sizeof(s_last_screen) - 1);
        s_last_screen[sizeof(s_last_screen) - 1] = 0;

        s_ui_dirty = true;

        if (len > 0) {
            ble_log_append_wrapped(rxline, 18);
        } else {
            // OPTIONAL: don't spam "(no rx)" every time, only when state changes.
            // If you keep it here, it will fill the log quickly.
            // ble_log_push_line("(no rx)");
        }
    }

    // Draw only when needed
    if (!s_ui_dirty) return;

    s_last_draw_ms = t;
    s_ui_dirty = false;

    draw_run_screen(CommBle_GetName(), addr, st_s);
}

const Experiment g_exp_ble = {
    .id = 8,
    .title = "BLE",
    .on_enter = ble_on_enter,
    .on_exit = ble_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};