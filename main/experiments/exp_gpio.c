#include "experiments/experiment.h"
#include "ui/ui.h"

#include "driver/gpio.h"
#include "esp_timer.h"

#include <stdio.h>
#include <stdbool.h>

// -------------------- GPIO mapping --------------------

#define PIN_RED    GPIO_NUM_13
#define PIN_GREEN  GPIO_NUM_14
#define PIN_YELLOW GPIO_NUM_1

// -------------------- state --------------------

static bool s_red_on = false;
static bool s_green_on = false;
static bool s_yellow_on = false;

static int s_sel = 0;              // 0..2
static bool s_ui_dirty = true;
static uint32_t s_last_draw_ms = 0;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// -------------------- gpio helpers --------------------

static void draw_full(void)
{
    Ui_DrawFrame("GPIO", "DN:NEXT  OK:TOGGLE  BACK");
    Ui_DrawGpioBody(s_sel, s_red_on, s_green_on, s_yellow_on);
}

static void gpio_apply_outputs(void)
{
    gpio_set_level(PIN_RED,    s_red_on ? 1 : 0);
    gpio_set_level(PIN_GREEN,  s_green_on ? 1 : 0);
    gpio_set_level(PIN_YELLOW, s_yellow_on ? 1 : 0);
}

static void gpio_set_outputs_mode(void)
{
    gpio_config_t io = {0};
    io.intr_type = GPIO_INTR_DISABLE;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_down_en = 0;
    io.pull_up_en = 0;
    io.pin_bit_mask = (1ULL << PIN_RED) | (1ULL << PIN_GREEN) | (1ULL << PIN_YELLOW);
    gpio_config(&io);
}

static void gpio_set_inputs_mode(void)
{
    gpio_config_t io = {0};
    io.intr_type = GPIO_INTR_DISABLE;
    io.mode = GPIO_MODE_INPUT;
    io.pull_down_en = 0;
    io.pull_up_en = 0;
    io.pin_bit_mask = (1ULL << PIN_RED) | (1ULL << PIN_GREEN) | (1ULL << PIN_YELLOW);
    gpio_config(&io);
}

static const char* onoff(bool v) { return v ? "ON" : "OFF"; }

// -------------------- UI drawing --------------------

static void draw_row(int idx, const char* label, int pin, bool on, bool selected)
{
    char line[48];
    // Example: "> RED    GPIO13  [ON]"
    snprintf(line, sizeof(line),
             "%c %-6s GPIO%-2d  [%s]",
             selected ? '>' : ' ',
             label,
             pin,
             onoff(on));
    Ui_Println(line);
}

static void draw_gpio_page(void)
{
    Ui_DrawFrame("GPIO", "DN:NEXT  OK:TOGGLE  BACK");

    Ui_Println("LED + SWITCH (3CH)");
    Ui_Println("");

    draw_row(0, "RED",    13, s_red_on,    s_sel == 0);
    draw_row(1, "GREEN",  14, s_green_on,  s_sel == 1);
    draw_row(2, "YELLOW",  1, s_yellow_on, s_sel == 2);

    Ui_Println("");
    Ui_Println("ON=HIGH  OFF=LOW");
}

// -------------------- experiment hooks --------------------

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("GPIO", "OK:START  BACK");
    Ui_Println("3 outputs:");
    Ui_Println("RED   -> GPIO13");
    Ui_Println("GREEN -> GPIO14");
    Ui_Println("YELLOW-> GPIO1");
    Ui_Println("");
    Ui_Println("DN select, OK toggle");
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;

    // Requirement: set outputs on enter
    gpio_set_outputs_mode();

    // Default OFF
    s_red_on = false;
    s_green_on = false;
    s_yellow_on = false;
    gpio_apply_outputs();

    s_sel = 0;
    s_ui_dirty = true;
}

static void exp_on_exit(ExperimentContext* ctx)
{
    (void)ctx;

    // Requirement: set inputs on exit
    gpio_set_inputs_mode();
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;

    // Ensure outputs mode in case framework differs
    gpio_set_outputs_mode();

    // Keep current states (or reset here if you prefer)
    gpio_apply_outputs();

    s_ui_dirty = true;

    Ui_LcdLock();
    Ui_DrawFrame("GPIO", "DN:NEXT  OK:TOGGLE  BACK");
    Ui_DrawGpioBody(s_sel, s_red_on, s_green_on, s_yellow_on);
    Ui_LcdUnlock();

}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;

    // Some frameworks call stop before exit
    gpio_set_inputs_mode();
    s_ui_dirty = true;
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;

    bool changed = false;

    if (key == kInputDown) {
        s_sel = (s_sel + 1) % 3;
        changed = true;
    } else if (key == kInputEnter) {
        if (s_sel == 0) s_red_on = !s_red_on;
        else if (s_sel == 1) s_green_on = !s_green_on;
        else s_yellow_on = !s_yellow_on;

        gpio_apply_outputs();
        changed = true;
    } else {
        return;
    }

    if (changed) {
        // Only redraw BODY, header/footer stay untouched
        Ui_DrawGpioBody(s_sel, s_red_on, s_green_on, s_yellow_on);
    }
}

static void tick(ExperimentContext* ctx) { (void)ctx; }


const Experiment g_exp_gpio = {
    .id = 1,
    .title = "GPIO",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};