#include "experiments/experiment.h"
#include "ui/ui.h"
#include "display/st7735.h"

#include "esp_log.h"

static const char* TAG = "EXP_LCD_COLOR";

static int s_sel = 0;
static bool s_sw_invert = false;
static bool s_sw_rb_swap = false;
static bool s_hw_invert = false;

static void apply_modes(void)
{
    St7735_SetSoftwareInvert(s_sw_invert);
    St7735_SetSoftwareRBSwap(s_sw_rb_swap);
    St7735_SetInversion(s_hw_invert);
}

static void draw_full(void)
{
    Ui_LcdLock();
    Ui_DrawFrame("LCD TEST", "DN:NEXT  OK:TOGGLE  BACK");
    Ui_DrawColorTestBody(s_sel, s_sw_invert, s_sw_rb_swap, s_hw_invert);
    Ui_LcdUnlock();
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("LCD TEST", "OK:START  BACK");
    Ui_Println("Color order & invert");
    Ui_Println("Use DN/OK to adjust.");
    Ui_Println("Find correct colors.");
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_enter");

    s_sel = 0;
    s_sw_invert = St7735_GetSoftwareInvert();
    s_sw_rb_swap = St7735_GetSoftwareRBSwap();
    s_hw_invert = St7735_GetInversion();

    apply_modes();
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
    draw_full();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "stop");
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    bool changed = false;

    if (key == kInputDown) {
        s_sel = (s_sel + 1) % 3;
        changed = true;
    } else if (key == kInputEnter) {
        if (s_sel == 0) s_sw_invert = !s_sw_invert;
        else if (s_sel == 1) s_sw_rb_swap = !s_sw_rb_swap;
        else if (s_sel == 2) s_hw_invert = !s_hw_invert;

        apply_modes();
        changed = true;
    } else {
        return;
    }

    if (changed) {
        draw_full();
    }
}

static void tick(ExperimentContext* ctx) { (void)ctx; }

const Experiment g_exp_lcd_color = {
    .id = 13,
    .title = "LCD TEST",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
