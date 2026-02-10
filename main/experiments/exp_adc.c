#include "experiments/experiment.h"
#include "ui/ui.h"

#include <stdio.h>
#include <string.h>

#include "driver/adc.h"
#include "esp_log.h"
#include "esp_timer.h"

#define ADC_GPIO 17

static const char* TAG = "EXP_ADC";

static bool s_adc_ok = false;
static adc2_channel_t s_adc_ch = ADC2_CHANNEL_6; // GPIO17 on ESP32-S3 ADC2

static uint32_t s_next_ms = 0;
static char s_line_raw[32];
static char s_line_v[32];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void adc_init_once(void)
{
    // Legacy ADC API (no esp_driver_adc dependency)
    adc2_config_channel_atten(s_adc_ch, ADC_ATTEN_DB_11);
    s_adc_ok = true;
}

static void render_value(int raw)
{
    // Approximate conversion (no calibration)
    float v = (float)raw * 3.3f / 4095.0f;

    char line_raw[32];
    char line_v[32];
    snprintf(line_raw, sizeof(line_raw), "RAW: %4d", raw);
    snprintf(line_v, sizeof(line_v), "V:   %.2f", v);

    if (strcmp(line_raw, s_line_raw) != 0) {
        strncpy(s_line_raw, line_raw, sizeof(s_line_raw) - 1);
        s_line_raw[sizeof(s_line_raw) - 1] = 0;
        Ui_DrawBodyTextRowColor(1, s_line_raw, Ui_ColorRGB(230, 230, 230));
    }
    if (strcmp(line_v, s_line_v) != 0) {
        strncpy(s_line_v, line_v, sizeof(s_line_v) - 1);
        s_line_v[sizeof(s_line_v) - 1] = 0;
        Ui_DrawBodyTextRowColor(2, s_line_v, Ui_ColorRGB(180, 220, 180));
    }
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("ADC", "BACK=RET");
    Ui_DrawBodyClear();
    Ui_DrawBodyTextRowColor(0, "AO -> GPIO17", Ui_ColorRGB(200, 200, 200));
    Ui_DrawBodyTextRowColor(1, "REFRESH: 10s", Ui_ColorRGB(200, 200, 200));
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    s_line_raw[0] = 0;
    s_line_v[0] = 0;
    s_next_ms = 0;
    adc_init_once();

    Ui_DrawFrame("ADC", "BACK=RET");
    Ui_DrawBodyClear();
    Ui_DrawBodyTextRowColor(0, "STATUS: RUN", Ui_ColorRGB(200, 200, 200));
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;

    uint32_t t = now_ms();
    if (t < s_next_ms) return;
    s_next_ms = t + 10000;

    if (!s_adc_ok) {
        Ui_DrawBodyTextRowColor(1, "ADC ERROR", Ui_ColorRGB(255, 120, 120));
        return;
    }

    int raw = 0;
    if (adc2_get_raw(s_adc_ch, ADC_WIDTH_BIT_12, &raw) != ESP_OK) {
        Ui_DrawBodyTextRowColor(1, "READ ERROR", Ui_ColorRGB(255, 120, 120));
        return;
    }

    render_value(raw);
}

const Experiment g_exp_adc = {
    .id = 3,
    .title = "ADC",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = 0,
    .on_key = 0,
    .tick = tick,
};
