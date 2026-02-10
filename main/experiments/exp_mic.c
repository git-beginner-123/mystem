#include "experiments/experiment.h"
#include "ui/ui.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
#include <string.h>

// -------------------- MIC (INMP441) --------------------
// WS  -> GPIO4
// SCK -> GPIO5
// SD  -> GPIO6
// LR  -> GND (LEFT)

#define MIC_I2S_PORT      I2S_NUM_0
#define MIC_SAMPLE_RATE   16000
#define MIC_BITS          I2S_DATA_BIT_WIDTH_32BIT

#define MIC_WS_GPIO       4
#define MIC_SCK_GPIO      5
#define MIC_SD_GPIO       6

#define MIC_SAMPLES       256
#define MIC_BANDS         10
#define MIC_UI_PERIOD_MS  2000
#define MIC_UI_SMOOTH_SHIFT 2
#define MIC_UI_VOL_SMOOTH_SHIFT 3

static const char* TAG = "EXP_MIC";

static bool s_running = false;
static i2s_chan_handle_t s_rx_chan = NULL;
static int32_t s_i2s_buf[MIC_SAMPLES];
static int16_t s_wave_buf[MIC_SAMPLES];
static int s_band_levels[MIC_BANDS];
static uint32_t s_last_ui_ms = 0;
static int s_vol_smooth = 0;
static int s_band_smooth[MIC_BANDS];

static void mic_start_driver(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(MIC_BITS, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK_GPIO,
            .ws = MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_SD_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
}

static void mic_stop_driver(void)
{
    if (s_rx_chan) {
        i2s_channel_disable(s_rx_chan);
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
    }
}

static int calc_freq_zero_cross(const int16_t* s, int n, int sample_rate)
{
    if (!s || n < 4) return 0;

    int crossings = 0;
    int prev = s[0];
    for (int i = 1; i < n; i++) {
        int cur = s[i];
        if ((prev <= 0 && cur > 0) || (prev >= 0 && cur < 0)) crossings++;
        prev = cur;
    }

    float seconds = (float)n / (float)sample_rate;
    float freq = (crossings / 2.0f) / seconds;
    if (freq < 0) freq = 0;
    if (freq > 20000) freq = 20000;
    return (int)(freq + 0.5f);
}

static int calc_volume_pct(const int16_t* s, int n)
{
    if (!s || n <= 0) return 0;

    int64_t sum_sq = 0;
    for (int i = 0; i < n; i++) {
        int32_t v = s[i];
        sum_sq += (int64_t)v * v;
    }
    float rms = sqrtf((float)sum_sq / (float)n);
    float db = 20.0f * log10f((rms / 32768.0f) + 1e-6f);
    float pct = (db + 50.0f) * (100.0f / 50.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (int)(pct + 0.5f);
}

static void calc_octave_bands(const int16_t* s, int n, int sample_rate, int* out_levels, int out_count)
{
    if (!s || n <= 0 || !out_levels || out_count <= 0) return;

    static const int centers[MIC_BANDS] = { 63, 125, 250, 500, 1000, 2000, 3000, 4000, 6000, 8000 };
    float max_mag = 1.0f;

    for (int b = 0; b < out_count; b++) {
        float freq = (float)centers[b];
        float w = 2.0f * 3.1415926f * freq / (float)sample_rate;
        float cosw = cosf(w);
        float coeff = 2.0f * cosw;

        float q0 = 0.0f;
        float q1 = 0.0f;
        float q2 = 0.0f;

        for (int i = 0; i < n; i++) {
            q0 = coeff * q1 - q2 + (float)s[i];
            q2 = q1;
            q1 = q0;
        }

        float mag = q1 * q1 + q2 * q2 - coeff * q1 * q2;
        if (mag < 0) mag = 0;
        if (mag > max_mag) max_mag = mag;
        out_levels[b] = (int)mag;
    }

    for (int b = 0; b < out_count; b++) {
        float mag = (float)out_levels[b];
        float db = 10.0f * log10f(mag + 1.0f);
        float pct = (db - 20.0f) * (100.0f / 40.0f);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        out_levels[b] = (int)(pct + 0.5f);
    }
}

static int peak_band_freq(const int* bands, int band_count)
{
    static const int centers[MIC_BANDS] = { 63, 125, 250, 500, 1000, 2000, 3000, 4000, 6000, 8000 };
    int best = 0;
    for (int i = 1; i < band_count; i++) {
        if (bands[i] > bands[best]) best = i;
    }
    return centers[best];
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("MIC", "OK:START  BACK");
    Ui_Println("INMP441 I2S");
    Ui_Println("WS  -> GPIO4");
    Ui_Println("SCK -> GPIO5");
    Ui_Println("SD  -> GPIO6");
    Ui_Println("LR  -> GND (LEFT)");
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_enter");
}

static void exp_on_exit(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_exit");
    if (s_running) {
        mic_stop_driver();
        s_running = false;
    }
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "start");
    mic_start_driver();
    s_running = true;

    Ui_DrawFrame("MIC", "BACK");
    Ui_DrawMicBody(NULL, 0, 0, 0);
    s_last_ui_ms = 0;
    s_vol_smooth = 0;
    for (int i = 0; i < MIC_BANDS; i++) s_band_smooth[i] = 0;
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "stop");
    if (s_running) {
        mic_stop_driver();
        s_running = false;
    }
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (key == kInputBack) return;
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
    if (!s_running) return;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (s_last_ui_ms && (now_ms - s_last_ui_ms) < MIC_UI_PERIOD_MS) {
        return;
    }

    size_t bytes_read = 0;
    esp_err_t r = i2s_channel_read(s_rx_chan, s_i2s_buf, sizeof(s_i2s_buf), &bytes_read, 0);
    if (r != ESP_OK || bytes_read == 0) return;

    int n = (int)(bytes_read / sizeof(int32_t));
    if (n <= 0) return;

    int64_t sum = 0;
    for (int i = 0; i < n; i++) {
        int32_t v = s_i2s_buf[i] >> 8; // 24-bit signed
        sum += v;
    }
    int32_t mean = (int32_t)(sum / n);

    int out_n = n;
    if (out_n > MIC_SAMPLES) out_n = MIC_SAMPLES;

    for (int i = 0; i < out_n; i++) {
        int32_t v = (s_i2s_buf[i] >> 8) - mean;
        s_wave_buf[i] = (int16_t)(v >> 7);
    }

    int vol = calc_volume_pct(s_wave_buf, out_n);
    s_vol_smooth += (vol - s_vol_smooth) >> MIC_UI_VOL_SMOOTH_SHIFT;
    vol = s_vol_smooth;
    calc_octave_bands(s_wave_buf, out_n, MIC_SAMPLE_RATE, s_band_levels, MIC_BANDS);
    for (int i = 0; i < MIC_BANDS; i++) {
        s_band_smooth[i] += (s_band_levels[i] - s_band_smooth[i]) >> MIC_UI_SMOOTH_SHIFT;
        s_band_levels[i] = s_band_smooth[i];
    }
    int freq = peak_band_freq(s_band_levels, MIC_BANDS);

    Ui_LcdLock();
    Ui_DrawMicBody(s_band_levels, MIC_BANDS, freq, vol);
    Ui_LcdUnlock();
    s_last_ui_ms = now_ms;
}

const Experiment g_exp_mic = {
    .id = 6,
    .title = "MIC",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
