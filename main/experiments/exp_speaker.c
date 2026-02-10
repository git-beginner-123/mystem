#include "experiments/experiment.h"
#include "ui/ui.h"

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdint.h>

// -------------------- MAX98357 --------------------
// DIN  -> GPIO7
// BCLK -> GPIO15
// LRCLK-> GPIO16

#define SPK_I2S_PORT     I2S_NUM_1
#define SPK_SAMPLE_RATE  16000
#define SPK_BITS         I2S_DATA_BIT_WIDTH_16BIT
#define SPK_DIN_GPIO     7
#define SPK_BCLK_GPIO    15
#define SPK_LRCLK_GPIO   16

extern const uint8_t _binary_hola_es_pcm_start[] asm("_binary_hola_es_pcm_start");
extern const uint8_t _binary_hola_es_pcm_end[]   asm("_binary_hola_es_pcm_end");

static const char* TAG = "EXP_SPK";

static i2s_chan_handle_t s_tx_chan = NULL;
static TaskHandle_t s_play_task = NULL;
static bool s_running = false;
static bool s_playing = false;
static volatile bool s_stop = false;
static int s_vol_pct = 100;
static int32_t s_gain_q15 = 0;

#define VOL_STEP_PCT 5

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void update_gain_q15(void)
{
    // -9 dB default attenuation: gain = 10^(-9/20) ≈ 0.3548
    const float base = 0.354813f;
    float g = base * (s_vol_pct / 100.0f);
    int32_t q15 = (int32_t)(g * 32768.0f);
    if (q15 < 0) q15 = 0;
    if (q15 > 32767) q15 = 32767;
    s_gain_q15 = q15;
}

static void spk_start_driver(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_I2S_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(SPK_BITS, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK_GPIO,
            .ws = SPK_LRCLK_GPIO,
            .dout = SPK_DIN_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
}

static void spk_stop_driver(void)
{
    if (s_tx_chan) {
        i2s_channel_disable(s_tx_chan);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
    }
}

static void spk_play_task(void* arg)
{
    (void)arg;
    const uint8_t* p = _binary_hola_es_pcm_start;
    size_t len = (size_t)(_binary_hola_es_pcm_end - _binary_hola_es_pcm_start);
    const size_t chunk = 2048;
    int16_t temp[chunk / 2];

    while (!s_stop && len > 0) {
        size_t n = (len > chunk) ? chunk : len;
        size_t written = 0;
        int samples = (int)(n / 2);
        const int16_t* in = (const int16_t*)p;
        for (int i = 0; i < samples; i++) {
            int32_t v = (int32_t)in[i] * s_gain_q15;
            v >>= 15;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            temp[i] = (int16_t)v;
        }
        esp_err_t r = i2s_channel_write(s_tx_chan, temp, (size_t)(samples * 2), &written, portMAX_DELAY);
        if (r != ESP_OK || written == 0) break;
        p += written;
        len -= written;
    }

    s_playing = false;
    s_play_task = NULL;
    vTaskDelete(NULL);
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("SPK", "OK:START  BACK");
    Ui_Println("MAX98357 I2S");
    Ui_Println("DIN  -> GPIO7");
    Ui_Println("BCLK -> GPIO15");
    Ui_Println("LRCLK-> GPIO16");
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
        s_stop = true;
        vTaskDelay(pdMS_TO_TICKS(20));
        if (s_play_task) {
            vTaskDelete(s_play_task);
            s_play_task = NULL;
        }
        s_playing = false;
        spk_stop_driver();
        s_running = false;
    }
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "start");

    if (s_running) return;

    spk_start_driver();
    s_stop = false;
    s_running = true;
    s_playing = false;
    s_vol_pct = 100;
    update_gain_q15();

    Ui_DrawFrame("SPK", "DN:-  UP:+  OK:PLAY  BACK");
    Ui_DrawSpeakerBody(s_playing, s_vol_pct);
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "stop");
    if (!s_running) return;

    s_stop = true;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (s_play_task) {
        vTaskDelete(s_play_task);
        s_play_task = NULL;
    }
    s_playing = false;
    spk_stop_driver();
    s_running = false;
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (!s_running) return;

    bool changed = false;

    if (key == kInputUp) {
        s_vol_pct = clamp_int(s_vol_pct + VOL_STEP_PCT, 0, 100);
        update_gain_q15();
        changed = true;
    } else if (key == kInputDown) {
        s_vol_pct = clamp_int(s_vol_pct - VOL_STEP_PCT, 0, 100);
        update_gain_q15();
        changed = true;
    } else if (key == kInputEnter) {
        if (s_playing) {
            s_stop = true;
            vTaskDelay(pdMS_TO_TICKS(20));
            if (s_play_task) {
                vTaskDelete(s_play_task);
                s_play_task = NULL;
            }
            s_playing = false;
        } else {
            s_stop = false;
            s_playing = true;
            xTaskCreate(spk_play_task, "spk_play", 4096, NULL, 5, &s_play_task);
        }
        changed = true;
    } else if (key == kInputBack) {
        return;
    }

    if (changed) {
        Ui_DrawFrame("SPK", "DN:-  UP:+  OK:PLAY  BACK");
        Ui_DrawSpeakerBody(s_playing, s_vol_pct);
    }
}

static void tick(ExperimentContext* ctx) { (void)ctx; }

const Experiment g_exp_speaker = {
    .id = 7,
    .title = "SPK",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
