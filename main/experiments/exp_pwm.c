#include "experiments/experiment.h"
#include "ui/ui.h"

#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"

// -------------------- GPIO mapping --------------------

#define PIN_RED    GPIO_NUM_13
#define PIN_GREEN  GPIO_NUM_14
#define PIN_YELLOW GPIO_NUM_1

// -------------------- PWM config --------------------

#define PWM_MODE          LEDC_LOW_SPEED_MODE
#define PWM_TIMER         LEDC_TIMER_0
#define PWM_RES_BITS      LEDC_TIMER_10_BIT
#define PWM_MAX_DUTY      ((1 << 10) - 1)
#define PWM_FREQ_MIN_HZ   200
#define PWM_FREQ_MAX_HZ   2000
#define PWM_FREQ_STEP_HZ  100
#define PWM_DUTY_STEP_PCT 5

static const char* TAG = "EXP_PWM";

static const int s_pwm_pins[3] = { PIN_RED, PIN_GREEN, PIN_YELLOW };
static const ledc_channel_t s_pwm_ch[3] = { LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2 };

static int s_sel = 0; // 0..3 (R,G,Y,FREQ)
static int s_duty_pct[3] = { 50, 50, 50 };
static int s_freq_hz = 1000;
static bool s_ui_inited = false;

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint32_t duty_raw(int pct)
{
    pct = clamp_int(pct, 0, 100);
    return (uint32_t)((pct * PWM_MAX_DUTY) / 100);
}

static void pwm_channels_apply(bool init_channels)
{
    for (int i = 0; i < 3; ++i) {
        if (init_channels) {
            ledc_channel_config_t c = {
                .gpio_num = s_pwm_pins[i],
                .speed_mode = PWM_MODE,
                .channel = s_pwm_ch[i],
                .timer_sel = PWM_TIMER,
                .duty = duty_raw(s_duty_pct[i]),
                .hpoint = 0,
                .intr_type = LEDC_INTR_DISABLE,
            };
            ESP_ERROR_CHECK(ledc_channel_config(&c));
        } else {
            ESP_ERROR_CHECK(ledc_set_duty(PWM_MODE, s_pwm_ch[i], duty_raw(s_duty_pct[i])));
            ESP_ERROR_CHECK(ledc_update_duty(PWM_MODE, s_pwm_ch[i]));
        }
    }
}

static void pwm_apply_all(bool init_channels)
{
    ledc_timer_config_t tcfg = {
        .speed_mode = PWM_MODE,
        .timer_num = PWM_TIMER,
        .duty_resolution = PWM_RES_BITS,
        .freq_hz = (uint32_t)s_freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));
    pwm_channels_apply(init_channels);
}

static void pwm_set_freq(int freq_hz)
{
    ESP_ERROR_CHECK(ledc_set_freq(PWM_MODE, PWM_TIMER, (uint32_t)freq_hz));
}

static void pwm_stop_all(void)
{
    for (int i = 0; i < 3; ++i) {
        ledc_stop(PWM_MODE, s_pwm_ch[i], 0);
        gpio_set_direction(s_pwm_pins[i], GPIO_MODE_INPUT);
    }
}

static void draw_full(void)
{
    Ui_LcdLock();
    Ui_DrawFrame("PWM", "UP:-  DN:NEXT  OK:+  BACK");
    Ui_DrawPwmBody(s_sel, s_duty_pct[0], s_duty_pct[1], s_duty_pct[2], s_freq_hz);
    Ui_LcdUnlock();
}

static void draw_body(void)
{
    Ui_LcdLock();
    Ui_DrawPwmBody(s_sel, s_duty_pct[0], s_duty_pct[1], s_duty_pct[2], s_freq_hz);
    Ui_LcdUnlock();
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("PWM", "OK:START  BACK");
    Ui_Println("PWM on 3 GPIOs");
    Ui_Println("RED   -> GPIO13");
    Ui_Println("GREEN -> GPIO14");
    Ui_Println("YELLOW-> GPIO1");
    Ui_Println("");
    Ui_Println("DN select  UP:-  OK:+");
    Ui_Println("R/G/Y=PWM  FREQ=HZ");
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_enter");
    s_sel = 0;
    s_duty_pct[0] = 50;
    s_duty_pct[1] = 50;
    s_duty_pct[2] = 50;
    s_freq_hz = 1000;
    s_ui_inited = false;
}

static void exp_on_exit(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_exit");
    pwm_stop_all();
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "start");
    pwm_apply_all(true);
    draw_full();
    s_ui_inited = true;
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "stop");
    pwm_stop_all();
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    bool changed = false;
    bool param_changed = false;

    if (key == kInputDown) {
        s_sel = (s_sel + 1) % 4;
        changed = true;
    } else if (key == kInputUp) {
        if (s_sel < 3) {
            s_duty_pct[s_sel] = clamp_int(s_duty_pct[s_sel] - PWM_DUTY_STEP_PCT, 0, 100);
        } else {
            s_freq_hz = clamp_int(s_freq_hz - PWM_FREQ_STEP_HZ, PWM_FREQ_MIN_HZ, PWM_FREQ_MAX_HZ);
        }
        changed = true;
        param_changed = true;
    } else if (key == kInputEnter) {
        if (s_sel < 3) {
            s_duty_pct[s_sel] = clamp_int(s_duty_pct[s_sel] + PWM_DUTY_STEP_PCT, 0, 100);
        } else {
            s_freq_hz = clamp_int(s_freq_hz + PWM_FREQ_STEP_HZ, PWM_FREQ_MIN_HZ, PWM_FREQ_MAX_HZ);
        }
        changed = true;
        param_changed = true;
    } else {
        return;
    }

    if (changed) {
        if (param_changed) {
            if (s_sel == 3) {
                pwm_set_freq(s_freq_hz);
            } else {
                pwm_channels_apply(false);
            }
        }
        if (!s_ui_inited) {
            draw_full();
            s_ui_inited = true;
        } else {
            draw_body();
        }
    }
}

static void tick(ExperimentContext* ctx) { (void)ctx; }

const Experiment g_exp_pwm = {
    .id = 2,
    .title = "PWM",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
