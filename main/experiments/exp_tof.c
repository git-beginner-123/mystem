#include "experiments/experiment.h"
#include "ui/ui.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TOF_TRIG_GPIO 39
#define TOF_ECHO_GPIO 2

static int64_t s_next_meas_us = 0;
static int s_last_cm = -1;
static bool s_valid = false;
static char s_last_line[32];
static uint32_t s_next_ui_ms = 0;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static uint32_t now_ms(void)
{
    return (uint32_t)(now_us() / 1000ULL);
}

static void send_trigger_pulse(void)
{
    gpio_set_level(TOF_TRIG_GPIO, 1);
    esp_rom_delay_us(10);
    gpio_set_level(TOF_TRIG_GPIO, 0);
}

static bool measure_once_cm(int* out_cm)
{
    if (!out_cm) return false;

    // Ensure trigger is low briefly
    gpio_set_level(TOF_TRIG_GPIO, 0);
    esp_rom_delay_us(2);

    send_trigger_pulse();

    int64_t t0 = now_us();
    const int64_t wait_high_us = 20000;
    const int64_t wait_low_us = 60000;

    while (gpio_get_level(TOF_ECHO_GPIO) == 0) {
        if ((now_us() - t0) > wait_high_us) return false;
    }

    int64_t t1 = now_us();
    while (gpio_get_level(TOF_ECHO_GPIO) == 1) {
        if ((now_us() - t1) > wait_low_us) return false;
    }

    int64_t pulse = now_us() - t1;
    if (pulse <= 0) return false;

    *out_cm = (int)(pulse / 58);
    return true;
}
static void update_ui(void)
{
    if (now_ms() < s_next_ui_ms) return;
    s_next_ui_ms = now_ms() + 200;

    char line[32];
    if (s_valid && s_last_cm >= 0 && s_last_cm <= 100) {
        snprintf(line, sizeof(line), "DIST: %3d cm", s_last_cm);
    } else {
        snprintf(line, sizeof(line), "DIST: UNKNOW");
    }

    if (strcmp(line, s_last_line) != 0) {
        strncpy(s_last_line, line, sizeof(s_last_line) - 1);
        s_last_line[sizeof(s_last_line) - 1] = 0;
        Ui_DrawBodyTextRowColor(1, line, Ui_ColorRGB(230, 230, 230));
    }
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("TOF", "BACK=RET");
    Ui_DrawBodyClear();
    Ui_DrawBodyTextRowColor(0, "TRIG=GPIO39", Ui_ColorRGB(200, 200, 200));
    Ui_DrawBodyTextRowColor(1, "ECHO=GPIO2", Ui_ColorRGB(200, 200, 200));
    Ui_DrawBodyTextRowColor(2, "UNIT: CM", Ui_ColorRGB(200, 200, 200));
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    // Delay GPIO init after power-up to avoid boot/flash interference
    vTaskDelay(pdMS_TO_TICKS(1500));
    gpio_config_t out_cfg = {
        .pin_bit_mask = 1ULL << TOF_TRIG_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t in_cfg = {
        .pin_bit_mask = 1ULL << TOF_ECHO_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);
    gpio_config(&in_cfg);
    gpio_set_level(TOF_TRIG_GPIO, 0);

    s_next_meas_us = now_us();
    s_last_cm = -1;
    s_valid = false;
    s_last_line[0] = 0;
    s_next_ui_ms = 0;

    Ui_DrawFrame("TOF", "BACK=RET");
    Ui_DrawBodyClear();
    Ui_DrawBodyTextRowColor(0, "STATUS: RUN", Ui_ColorRGB(200, 200, 200));
    Ui_DrawBodyTextRowColor(1, "DIST: UNKNOW", Ui_ColorRGB(230, 230, 230));
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    gpio_set_level(TOF_TRIG_GPIO, 0);
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;

    int64_t t = now_us();

    if (t >= s_next_meas_us) {
        int cm = -1;
        if (measure_once_cm(&cm)) {
            s_last_cm = cm;
            s_valid = (cm >= 0 && cm <= 100);
        } else {
            s_valid = false;
        }
        s_next_meas_us = now_us() + 200000;
    }

    update_ui();
}

const Experiment g_exp_tof = {
    .id = 5,
    .title = "TOF",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = 0,
    .tick = tick,
};
