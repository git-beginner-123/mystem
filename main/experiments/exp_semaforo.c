#include "experiments/experiment.h"
#include "ui/ui.h"

#include <stdio.h>

#include "display/st7735.h"
#include "esp_timer.h"

typedef enum {
    kLightRed = 0,
    kLightYellow,
    kLightGreen
} LightState;

typedef struct {
    LightState ns;
    LightState ew;
    int remain_s;
} PhaseState;

static int s_phase = 0;
static int s_remain = 0;
static int s_last_phase = -1;
static int s_last_remain = -1;
static uint32_t s_next_ms = 0;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint16_t color_off(void)   { return Ui_ColorRGB(40, 40, 40); }
static uint16_t color_red(void)   { return Ui_ColorRGB(220, 40, 40); }
static uint16_t color_yel(void)   { return Ui_ColorRGB(230, 200, 40); }
static uint16_t color_grn(void)   { return Ui_ColorRGB(40, 200, 60); }
static uint16_t color_road(void)  { return Ui_ColorRGB(30, 30, 35); }
static uint16_t color_text(void)  { return Ui_ColorRGB(230, 230, 230); }
static uint16_t color_bg(void)    { return Ui_ColorRGB(8, 14, 20); }

static void draw_light_box(int x, int y, LightState state)
{
    int lamp = 14;
    int gap = 4;
    int box_pad = 4;
    int box_w = lamp + box_pad * 2;
    int box_h = (lamp * 3) + (gap * 2) + box_pad * 2;

    St7735_FillRect(x, y, box_w, box_h, Ui_ColorRGB(15, 15, 20));
    // simple border
    St7735_FillRect(x, y, box_w, 1, Ui_ColorRGB(80, 80, 90));
    St7735_FillRect(x, y + box_h - 1, box_w, 1, Ui_ColorRGB(80, 80, 90));
    St7735_FillRect(x, y, 1, box_h, Ui_ColorRGB(80, 80, 90));
    St7735_FillRect(x + box_w - 1, y, 1, box_h, Ui_ColorRGB(80, 80, 90));

    int lx = x + box_pad;
    int ly = y + box_pad;
    St7735_FillRect(lx, ly, lamp, lamp, state == kLightRed ? color_red() : color_off());
    St7735_FillRect(lx, ly + lamp + gap, lamp, lamp, state == kLightYellow ? color_yel() : color_off());
    St7735_FillRect(lx, ly + (lamp + gap) * 2, lamp, lamp, state == kLightGreen ? color_grn() : color_off());
}

static PhaseState phase_state(int phase, int remain)
{
    PhaseState s;
    s.remain_s = remain;
    switch (phase) {
        case 0: s.ns = kLightGreen;  s.ew = kLightRed;   break;
        case 1: s.ns = kLightYellow; s.ew = kLightRed;   break;
        case 2: s.ns = kLightRed;    s.ew = kLightGreen; break;
        case 3: s.ns = kLightRed;    s.ew = kLightYellow;break;
        default: s.ns = kLightRed;   s.ew = kLightRed;   break;
    }
    return s;
}

static uint16_t light_color(LightState s)
{
    if (s == kLightRed) return color_red();
    if (s == kLightYellow) return color_yel();
    return color_grn();
}

static void draw_lights(const PhaseState* st)
{
    int w = St7735_Width();
    int h = St7735_Height();

    int body_y = 30;
    int body_h = h - 30 - 26;
    int body_x = 0;
    int body_w = w;

    // clear body
    St7735_FillRect(body_x, body_y, body_w, body_h, color_bg());

    int cx = w / 2;
    int cy = body_y + body_h / 2;
    int road_w = 60;
    int road_h = 60;

    // road square
    St7735_FillRect(cx - road_w / 2, cy - road_h / 2, road_w, road_h, color_road());
    // cross roads
    St7735_FillRect(cx - 12, body_y + 6, 24, body_h - 12, color_road());
    St7735_FillRect(body_x + 6, cy - 12, body_w - 12, 24, color_road());

    // light sizes
    int lamp = 14;
    int gap = 4;
    int box_pad = 4;
    int box_w = lamp + box_pad * 2;
    int box_h = (lamp * 3) + (gap * 2) + box_pad * 2;

    // North (top)
    int nx = cx - box_w / 2;
    int ny = body_y + 6;
    draw_light_box(nx, ny, st->ns);

    // South (bottom)
    int sx = cx - box_w / 2;
    int sy = body_y + body_h - box_h - 6;
    draw_light_box(sx, sy, st->ns);

    // West (left)
    int wx = body_x + 6;
    int wy = cy - box_h / 2;
    draw_light_box(wx, wy, st->ew);

    // East (right)
    int ex = body_x + body_w - box_w - 6;
    int ey = cy - box_h / 2;
    draw_light_box(ex, ey, st->ew);

    // Time labels near each side
    char tbuf[12];
    snprintf(tbuf, sizeof(tbuf), "%2ds", st->remain_s);

    // North time (right of box)
    Ui_DrawTextAtBg(nx + box_w + 4, ny + box_h / 2 - 8, tbuf, color_text(), color_bg());
    // South time (right of box)
    Ui_DrawTextAtBg(sx + box_w + 4, sy + box_h / 2 - 8, tbuf, color_text(), color_bg());
    // West time (left of box)
    Ui_DrawTextAtBg(wx + box_w + 4, wy + box_h / 2 - 8, tbuf, color_text(), color_bg());
    // East time (right of box)
    Ui_DrawTextAtBg(ex - 30, ey + box_h / 2 - 8, tbuf, color_text(), color_bg());

    St7735_Flush();
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("SEMAFORO", "BACK=RET");
    Ui_DrawBodyClear();
    Ui_DrawBodyTextRowColor(0, "LCD DEMO", color_text());
    Ui_DrawBodyTextRowColor(1, "NS/EW synced", color_text());
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    s_phase = 0;
    s_remain = 20;
    s_last_phase = -1;
    s_last_remain = -1;
    s_next_ms = 0;

    Ui_DrawFrame("SEMAFORO", "BACK=RET");
    Ui_DrawBodyClear();

    PhaseState st = phase_state(s_phase, s_remain);
    draw_lights(&st);
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;

    uint32_t t = now_ms();
    if (t < s_next_ms) return;
    s_next_ms = t + 1000;

    if (s_remain <= 0) {
        s_phase = (s_phase + 1) % 4;
        s_remain = (s_phase == 1 || s_phase == 3) ? 3 : 20;
    } else {
        s_remain--;
    }

    if (s_phase != s_last_phase || s_remain != s_last_remain) {
        PhaseState st = phase_state(s_phase, s_remain);
        draw_lights(&st);
        s_last_phase = s_phase;
        s_last_remain = s_remain;
    }
}

const Experiment g_exp_semaforo = {
    .id = 11,
    .title = "SEMAFORO",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = 0,
    .on_key = 0,
    .tick = tick,
};
