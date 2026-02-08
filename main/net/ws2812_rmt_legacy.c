#include "ws2812_rmt_legacy.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_rom_sys.h"

#include "driver/rmt.h"
#include "driver/gpio.h"

#define WS2812_GPIO        GPIO_NUM_48
#define WS2812_RMT_CH      RMT_CHANNEL_0
#define WS2812_CLK_DIV     8   // 80MHz/8=10MHz => 0.1us/tick
#define WS2812_RESET_US    80

static const char* TAG = "ws2812";

/* 0.1us per tick */
#define T0H 4
#define T0L 8
#define T1H 7
#define T1L 6

static bool s_inited = false;

static void build_items(const uint8_t grb[3], rmt_item32_t items[24])
{
    int idx = 0;
    for (int bi = 0; bi < 3; bi++) {
        uint8_t v = grb[bi];
        for (int bit = 7; bit >= 0; bit--) {
            bool one = ((v >> bit) & 1) != 0;
            if (one) {
                items[idx].level0 = 1;
                items[idx].duration0 = T1H;
                items[idx].level1 = 0;
                items[idx].duration1 = T1L;
            } else {
                items[idx].level0 = 1;
                items[idx].duration0 = T0H;
                items[idx].level1 = 0;
                items[idx].duration1 = T0L;
            }
            idx++;
        }
    }
}

bool Ws2812_Init(void)
{
    if (s_inited) return true;

    rmt_config_t cfg = {0};
    cfg.rmt_mode = RMT_MODE_TX;
    cfg.channel = WS2812_RMT_CH;
    cfg.gpio_num = WS2812_GPIO;
    cfg.clk_div = WS2812_CLK_DIV;
    cfg.mem_block_num = 1;
    cfg.tx_config.loop_en = false;
    cfg.tx_config.carrier_en = false;
    cfg.tx_config.idle_output_en = true;
    cfg.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;

    esp_err_t err = rmt_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_config failed: %d", (int)err);
        return false;
    }
    err = rmt_driver_install(cfg.channel, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_driver_install failed: %d", (int)err);
        return false;
    }

    s_inited = true;
    Ws2812_Off();
    return true;
}

void Ws2812_SetRgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!Ws2812_Init()) return;

    uint8_t grb[3] = { g, r, b };
    rmt_item32_t items[24];
    build_items(grb, items);

    rmt_write_items(WS2812_RMT_CH, items, 24, true);
    rmt_wait_tx_done(WS2812_RMT_CH, pdMS_TO_TICKS(50));
    esp_rom_delay_us(WS2812_RESET_US);
}

void Ws2812_Off(void)
{
    Ws2812_SetRgb(0, 0, 0);
}
