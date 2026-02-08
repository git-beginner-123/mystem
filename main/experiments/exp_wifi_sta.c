#include "experiments/experiment.h"
#include "ui/ui.h"

#include <string.h>
#include <stdio.h>

#include "esp_timer.h"
#include "net/stock_yahoo.h"

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static const char* kSymbols[] = {
    "ITX.MC", // Inditex
    "SAN.MC", // Santander
    "IBE.MC", // Iberdrola
};

static uint32_t s_next_ui_ms = 0;

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_Clear();
    Ui_Println("WIFI STA");
    Ui_Println("GOAL: fetch stocks");
    Ui_Println("SRC:  Yahoo chart");
    Ui_Println("PER:  30s/one symbol");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_Clear();
    Ui_Println("WIFI STA");
    Ui_Println("Preparing...");

    Stocks_Init(kSymbols, (int)(sizeof(kSymbols) / sizeof(kSymbols[0])));
    s_next_ui_ms = 0;

    Ui_Clear();
    Ui_Println("WIFI STA");
    Ui_Println("Stocks loading...");
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_Clear();
    Ui_Println("WIFI STA");
    Ui_Println("Stopped");
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;

    uint32_t t = now_ms();

    // Network fetch (internally throttled)
    Stocks_Tick(t);

    // UI refresh every 2s
    if (t < s_next_ui_ms) return;
    s_next_ui_ms = t + 2000;

    Ui_Clear();
    Ui_Println("WIFI STA - STOCKS");

    for (int i = 0; i < Stocks_Count(); i++) {
        StockQuote q;
        if (Stocks_Get(i, &q)) {
            Ui_Printf("%s %.2f  %+0.2f%%", q.symbol, q.price, q.change_pct);
        } else {
            Ui_Printf("%s ...", kSymbols[i]);
        }
    }

    Ui_Println("");
    Ui_Println("Refresh: 30s/symbol");
}

const Experiment g_exp_wifi_sta = {
    .id = 9,
    .title = "WIFI STA",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = 0,
    .tick = tick,
};
