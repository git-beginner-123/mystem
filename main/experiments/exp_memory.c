#include "experiments/experiment.h"
#include "ui/ui.h"
#include "display/st7789.h"
#include "audio/sfx.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static const char* TAG = "EXP_MEMORY";

#define UI_HEADER_H 30
#define UI_FOOTER_H 26
#define REVEAL_MS 5000U
#define MEM_MAX_CARDS 6
#define UP_COMBO_WINDOW_MS 1000U

#define KBD_COLS 6
#define KBD_ROWS 6
#define KBD_KEYS 36

typedef enum {
    kStateLevelSelect = 0,
    kStateReveal,
    kStateGuessDigits,
    kStateResult,
} MemState;

static const int kLevelCardCount[4] = {3, 4, 5, 6};
static const char kKeyChars[KBD_KEYS + 1] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

static int s_level_idx = 0;
static int s_card_count = 3;
static char s_cards[MEM_MAX_CARDS];
static char s_pick_digits[MEM_MAX_CARDS] = {'_', '_', '_', '_', '_', '_'};
static bool s_digit_match[MEM_MAX_CARDS] = {false, false, false, false, false, false};
static int s_digit_slot_focus = 0;
static uint32_t s_reveal_until_ms = 0;
static int s_last_countdown_sec = -1;
static int s_focus_idx = 18; // center key
static uint32_t s_last_up_key_ms = 0;
static bool s_up_combo_can_revert = false;
static MemState s_state = kStateLevelSelect;

static void draw_cards(bool face_up, int focus_idx);
static void draw_keyboard(void);
static void draw_guess_slots(void);

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint16_t c_bg(void) { return Ui_ColorRGB(10, 16, 28); }
static uint16_t c_card(void) { return Ui_ColorRGB(30, 46, 72); }
static uint16_t c_card_open(void) { return Ui_ColorRGB(42, 74, 112); }
static uint16_t c_text(void) { return Ui_ColorRGB(235, 235, 235); }
static uint16_t c_muted(void) { return Ui_ColorRGB(160, 180, 210); }
static uint16_t c_ok(void) { return Ui_ColorRGB(120, 230, 130); }
static uint16_t c_warn(void) { return Ui_ColorRGB(255, 130, 130); }
static uint16_t c_key_alpha(void) { return Ui_ColorRGB(35, 64, 98); }
static uint16_t c_key_digit(void) { return Ui_ColorRGB(54, 74, 40); }
static uint16_t c_key_focus(void) { return Ui_ColorRGB(255, 210, 80); }

static bool is_digit(char ch) { return (ch >= '0' && ch <= '9'); }
static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void clear_guess_state(void)
{
    for (int i = 0; i < MEM_MAX_CARDS; i++) {
        s_pick_digits[i] = '_';
        s_digit_match[i] = false;
    }
    s_digit_slot_focus = 0;
}

static void move_keyboard_up(void)
{
    s_focus_idx = (s_focus_idx + KBD_KEYS - KBD_COLS) % KBD_KEYS;
    draw_keyboard();
    St7789_Flush();
}

static void move_keyboard_down(void)
{
    s_focus_idx = (s_focus_idx + KBD_COLS) % KBD_KEYS;
    draw_keyboard();
    St7789_Flush();
}

static void move_slot_focus_left(void)
{
    s_digit_slot_focus = (s_digit_slot_focus + s_card_count - 1) % s_card_count;
    draw_guess_slots();
    draw_cards(false, s_digit_slot_focus);
    St7789_Flush();
}

static void move_slot_focus_right(void)
{
    s_digit_slot_focus = (s_digit_slot_focus + 1) % s_card_count;
    draw_guess_slots();
    draw_cards(false, s_digit_slot_focus);
    St7789_Flush();
}

static void card_xy(int idx, int count, int* out_x, int* out_y, int* out_w, int* out_h)
{
    int w = 32;
    int h = 42;
    int gap = 6;
    int area_top = UI_HEADER_H + 52;
    int total = count * w + (count - 1) * gap;
    int x0 = (St7789_Width() - total) / 2;
    *out_x = x0 + idx * (w + gap);
    *out_y = area_top + 8;

    *out_w = w;
    *out_h = h;
}

static void keyboard_key_xy(int idx, int* out_x, int* out_y, int* out_w, int* out_h)
{
    int w = 30;
    int h = 16;
    int gap = 1;
    int total_w = KBD_COLS * w + (KBD_COLS - 1) * gap;
    int x0 = (St7789_Width() - total_w) / 2;
    int total_h = KBD_ROWS * h + (KBD_ROWS - 1) * gap;
    int y0 = (St7789_Height() - UI_FOOTER_H - 2) - total_h;

    int r = idx / KBD_COLS;
    int c = idx % KBD_COLS;

    *out_x = x0 + c * (w + gap);
    *out_y = y0 + r * (h + gap);
    *out_w = w;
    *out_h = h;
}

static void generate_cards(void)
{
    s_card_count = kLevelCardCount[s_level_idx];
    for (int i = 0; i < s_card_count; i++) {
        s_cards[i] = kKeyChars[(int)(esp_random() % KBD_KEYS)];
    }

    for (int i = s_card_count - 1; i > 0; i--) {
        int j = (int)((unsigned)esp_random() % (unsigned)(i + 1));
        char t = s_cards[i];
        s_cards[i] = s_cards[j];
        s_cards[j] = t;
    }
}

static void draw_cards(bool face_up, int focus_idx)
{
    for (int i = 0; i < s_card_count; i++) {
        int x, y, w, h;
        card_xy(i, s_card_count, &x, &y, &w, &h);
        bool focused = (!face_up && i == focus_idx);
        uint16_t edge = focused ? c_key_focus() : c_muted();
        St7789_FillRect(x, y, w, h, face_up ? c_card_open() : c_card());
        St7789_FillRect(x, y, w, 2, edge);
        St7789_FillRect(x, y + h - 2, w, 2, edge);
        St7789_FillRect(x, y, 2, h, edge);
        St7789_FillRect(x + w - 2, y, 2, h, edge);

        char text[2] = { face_up ? s_cards[i] : '?', '\0' };
        Ui_DrawTextAtBg(x + (w / 2) - 4, y + (h / 2) - 8, text, focused ? c_key_focus() : c_text(),
                        face_up ? c_card_open() : c_card());
    }
}

static void draw_keyboard(void)
{
    for (int i = 0; i < KBD_KEYS; i++) {
        int x, y, w, h;
        keyboard_key_xy(i, &x, &y, &w, &h);
        char ch = kKeyChars[i];
        bool focused = (i == s_focus_idx);

        uint16_t bg = is_digit(ch) ? c_key_digit() : c_key_alpha();
        uint16_t fg = c_text();
        if (focused) {
            bg = c_key_focus();
            fg = Ui_ColorRGB(20, 20, 20);
        }

        St7789_FillRect(x, y, w, h, bg);
        St7789_FillRect(x, y, w, 1, c_muted());
        St7789_FillRect(x, y + h - 1, w, 1, c_muted());
        St7789_FillRect(x, y, 1, h, c_muted());
        St7789_FillRect(x + w - 1, y, 1, h, c_muted());

        char t[2] = { ch, '\0' };
        Ui_DrawTextAtBg(x + (w / 2) - 4, y + 1, t, fg, bg);
    }
}

static void draw_countdown_sec(int sec)
{
    St7789_FillRect(0, UI_HEADER_H + 6, St7789_Width(), 20, c_bg());
    char line[32];
    snprintf(line, sizeof(line), "Countdown: %d", sec);
    Ui_DrawTextAtBg(8, UI_HEADER_H + 8, line, c_warn(), c_bg());
    St7789_Flush();
}

static void draw_guess_slots(void)
{
    int w = 28;
    int h = 26;
    int gap = 6;
    int total = s_card_count * w + (s_card_count - 1) * gap;
    int x0 = (St7789_Width() - total) / 2;
    int y0 = UI_HEADER_H + 28;

    for (int i = 0; i < s_card_count; i++) {
        int x = x0 + i * (w + gap);
        uint16_t bg = (i == s_digit_slot_focus) ? c_key_focus() : c_card_open();
        uint16_t fg = (i == s_digit_slot_focus) ? Ui_ColorRGB(20, 20, 20) : c_text();

        St7789_FillRect(x, y0, w, h, bg);
        St7789_FillRect(x, y0, w, 1, c_muted());
        St7789_FillRect(x, y0 + h - 1, w, 1, c_muted());
        St7789_FillRect(x, y0, 1, h, c_muted());
        St7789_FillRect(x + w - 1, y0, 1, h, c_muted());

        char t[2] = { s_pick_digits[i], '\0' };
        Ui_DrawTextAtBg(x + (w / 2) - 4, y0 + 5, t, fg, bg);
    }
}

static void draw_scene(void)
{
    const char* footer = "UP/DN/LR:KEY BACK:SLOT< OK";
    if (s_state == kStateLevelSelect) footer = "UP/DN:LV OK:OPEN BACK";
    if (s_state == kStateReveal) footer = "MEMORIZE 5S";
    if (s_state == kStateResult) footer = "UP/DN:LV OK:NEXT BACK";

    Ui_DrawFrame("MEMORY", footer);
    St7789_FillRect(0, UI_HEADER_H, St7789_Width(),
                    St7789_Height() - UI_HEADER_H - UI_FOOTER_H, c_bg());

    char line[64];
    if (s_state == kStateLevelSelect) {
        snprintf(line, sizeof(line), "Level %d  (%d cards)", s_level_idx + 1, s_card_count);
        Ui_DrawTextAtBg(8, UI_HEADER_H + 8, line, c_text(), c_bg());
        Ui_DrawTextAtBg(8, UI_HEADER_H + 28, "OK to reveal all cards (5s)", c_muted(), c_bg());
        draw_cards(false, -1);
    } else if (s_state == kStateReveal) {
        draw_cards(true, -1);
        draw_countdown_sec(5);
        s_last_countdown_sec = 5;
        return;
    } else if (s_state == kStateGuessDigits) {
        St7789_FillRect(0, UI_HEADER_H + 6, St7789_Width(), 20, c_bg());
        snprintf(line, sizeof(line), "Input %d chars in order", s_card_count);
        Ui_DrawTextAtBg(8, UI_HEADER_H + 8, line, c_text(), c_bg());
        draw_guess_slots();
        draw_cards(false, s_digit_slot_focus);
        draw_keyboard();
    } else {
        bool pass = true;
        for (int i = 0; i < s_card_count; i++) pass = pass && s_digit_match[i];
        int n = snprintf(line, sizeof(line), "%s  L%d  IN:", pass ? "PASS" : "FAIL", s_level_idx + 1);
        for (int i = 0; i < s_card_count && n < (int)sizeof(line) - 1; i++) {
            line[n++] = s_pick_digits[i];
        }
        if (n < (int)sizeof(line) - 6) {
            line[n++] = ' ';
            line[n++] = 'A';
            line[n++] = 'N';
            line[n++] = 'S';
            line[n++] = ':';
            for (int i = 0; i < s_card_count && n < (int)sizeof(line) - 1; i++) {
                line[n++] = s_cards[i];
            }
        }
        line[n] = '\0';
        Ui_DrawTextAtBg(8, UI_HEADER_H + 8, line, pass ? c_ok() : c_warn(), c_bg());
        Ui_DrawTextAtBg(8, UI_HEADER_H + 28, "UP/DN change level, OK next", c_muted(), c_bg());
        draw_cards(true, -1);
    }
    St7789_Flush();
}

static void begin_round(void)
{
    generate_cards();
    clear_guess_state();
    s_focus_idx = 18; // center key by default
    s_last_up_key_ms = 0;
    s_up_combo_can_revert = false;
    s_last_countdown_sec = -1;
    s_reveal_until_ms = now_ms() + REVEAL_MS;
    s_state = kStateReveal;
    draw_scene();
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("MEMORY", "OK:START  BACK");
    Ui_Println("Memory cards game.");
    Ui_Println("4 levels: 3/4/5/6 cards.");
    Ui_Println("Digits + letters in keypad.");
    Ui_Println("OK reveals cards for 5s.");
    Ui_Println("Then input full sequence.");
    Ui_Println("UP/DN/L/R: key");
    Ui_Println("BACK: move top focus left");
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_enter");
    s_level_idx = 0;
    s_card_count = kLevelCardCount[s_level_idx];
    s_state = kStateLevelSelect;
    s_focus_idx = 18;
    s_last_up_key_ms = 0;
    s_up_combo_can_revert = false;
    clear_guess_state();
}

static void exp_on_exit(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_exit");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "start");
    draw_scene();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "stop");
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (s_state == kStateReveal) return;

    if (s_state == kStateLevelSelect) {
        if (key == kInputUp) {
            s_level_idx = (s_level_idx + 3) % 4;
            s_card_count = kLevelCardCount[s_level_idx];
            clear_guess_state();
            draw_scene();
        } else if (key == kInputDown) {
            s_level_idx = (s_level_idx + 1) % 4;
            s_card_count = kLevelCardCount[s_level_idx];
            clear_guess_state();
            draw_scene();
        } else if (key == kInputEnter) {
            begin_round();
        }
        return;
    }

    if (s_state == kStateGuessDigits) {
        uint32_t tnow = now_ms();
        bool up_combo_active = ((tnow - s_last_up_key_ms) <= UP_COMBO_WINDOW_MS);

        if (key == kInputBack) {
            move_slot_focus_left();
            ctx->consume_back = true;
            return;
        }

        if (key == kInputLeft) {
            if (up_combo_active) {
                if (s_up_combo_can_revert) {
                    // Cancel the temporary keyboard-up step from UP.
                    move_keyboard_down();
                }
                s_up_combo_can_revert = false;
                move_slot_focus_left();
                return;
            }
            s_up_combo_can_revert = false;
            s_focus_idx = (s_focus_idx + KBD_KEYS - 1) % KBD_KEYS;
            draw_keyboard();
            St7789_Flush();
            return;
        }
        if (key == kInputRight) {
            if (up_combo_active) {
                if (s_up_combo_can_revert) {
                    // Cancel the temporary keyboard-up step from UP.
                    move_keyboard_down();
                }
                s_up_combo_can_revert = false;
                move_slot_focus_right();
                return;
            }
            s_up_combo_can_revert = false;
            s_focus_idx = (s_focus_idx + 1) % KBD_KEYS;
            draw_keyboard();
            St7789_Flush();
            return;
        }

        if (key == kInputUp) {
            s_last_up_key_ms = tnow;
            s_up_combo_can_revert = true;
            move_keyboard_up();
            return;
        }
        if (key == kInputDown) {
            s_up_combo_can_revert = false;
            move_keyboard_down();
            return;
        }
        if (key == kInputEnter) {
            s_last_up_key_ms = 0;
            s_up_combo_can_revert = false;
            char ch = kKeyChars[s_focus_idx];
            s_pick_digits[s_digit_slot_focus] = ch;
            if (s_digit_slot_focus < s_card_count - 1) {
                s_digit_slot_focus++;
            } else {
                for (int i = 0; i < s_card_count; i++) {
                    s_digit_match[i] = (s_pick_digits[i] == s_cards[i]);
                }
                s_state = kStateResult;
                bool pass = true;
                for (int i = 0; i < s_card_count; i++) pass = pass && s_digit_match[i];
                if (pass) Sfx_PlaySuccess();
            }
            draw_scene();
            return;
        }
    }

    if (s_state == kStateResult) {
        if (key == kInputUp) {
            s_level_idx = (s_level_idx + 3) % 4;
            s_card_count = kLevelCardCount[s_level_idx];
            clear_guess_state();
            s_state = kStateLevelSelect;
            draw_scene();
            return;
        }
        if (key == kInputDown) {
            s_level_idx = (s_level_idx + 1) % 4;
            s_card_count = kLevelCardCount[s_level_idx];
            clear_guess_state();
            s_state = kStateLevelSelect;
            draw_scene();
            return;
        }
        if (key == kInputEnter) {
            begin_round();
            return;
        }
    }
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
    if (s_state != kStateReveal) return;

    uint32_t t = now_ms();
    if (t >= s_reveal_until_ms) {
        s_state = kStateGuessDigits;
        s_focus_idx = 26; // start on '0'
        s_digit_slot_focus = 0; // focus first number box
        draw_scene();
        return;
    }

    uint32_t left_ms = s_reveal_until_ms - t;
    int sec = (int)((left_ms + 999U) / 1000U); // ceil
    if (sec != s_last_countdown_sec) {
        s_last_countdown_sec = sec;
        draw_countdown_sec(sec);
    }
}

const Experiment g_exp_memory = {
    .id = 23,
    .title = "MEMORY",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
