#include "stock_yahoo.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "esp_timer.h"

static const char* TAG = "stocks";

static StockQuote* s_list = NULL;
static int s_count = 0;

static uint32_t s_next_fetch_ms = 0;
static int s_next_index = 0;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static int http_get_all(const char* url, char* buf, int cap, int* out_status)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 6000,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;

    if (esp_http_client_open(c, 0) != ESP_OK) {
        esp_http_client_cleanup(c);
        return -1;
    }

    int status = esp_http_client_get_status_code(c);
    if (out_status) *out_status = status;

    int total = 0;
    while (total < cap - 1) {
        int n = esp_http_client_read(c, buf + total, cap - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = 0;

    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return total;
}

static bool parse_quote(const char* json, float* out_price, float* out_chg_pct)
{
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;

    bool ok = false;

    cJSON* chart  = cJSON_GetObjectItem(root, "chart");
    cJSON* result = chart ? cJSON_GetObjectItem(chart, "result") : NULL;
    cJSON* r0     = (result && cJSON_IsArray(result)) ? cJSON_GetArrayItem(result, 0) : NULL;
    cJSON* meta   = r0 ? cJSON_GetObjectItem(r0, "meta") : NULL;

    cJSON* price  = meta ? cJSON_GetObjectItem(meta, "regularMarketPrice") : NULL;
    cJSON* chg    = meta ? cJSON_GetObjectItem(meta, "regularMarketChangePercent") : NULL;

    if (cJSON_IsNumber(price) && cJSON_IsNumber(chg)) {
        *out_price = (float)price->valuedouble;
        *out_chg_pct = (float)chg->valuedouble;
        ok = true;
    }

    cJSON_Delete(root);
    return ok;
}

void Stocks_Init(const char* const* symbols, int count)
{
    if (s_list) {
        free(s_list);
        s_list = NULL;
    }
    s_count = 0;
    s_next_index = 0;
    s_next_fetch_ms = 0;

    if (!symbols || count <= 0) return;

    s_list = (StockQuote*)calloc((size_t)count, sizeof(StockQuote));
    if (!s_list) return;

    s_count = count;
    for (int i = 0; i < count; i++) {
        strncpy(s_list[i].symbol, symbols[i], sizeof(s_list[i].symbol) - 1);
        s_list[i].valid = false;
    }
}

void Stocks_Tick(uint32_t unused_now_ms)
{
    (void)unused_now_ms;

    if (!s_list || s_count <= 0) return;

    uint32_t t = now_ms();
    if (t < s_next_fetch_ms) return;

    // Fetch one symbol each period (reduces TLS load / rate limit risk)
    s_next_fetch_ms = t + 30000; // 30s
    int idx = s_next_index++ % s_count;

    char url[160];
    snprintf(url, sizeof(url),
             "https://query1.finance.yahoo.com/v8/finance/chart/%s",
             s_list[idx].symbol);

    static char* buf = NULL;
    static int cap = 0;
    if (!buf) { cap = 8 * 1024; buf = (char*)malloc(cap); }
    if (!buf) return;

    int status = 0;
    int n = http_get_all(url, buf, cap, &status);
    if (n <= 0 || status != 200) {
        ESP_LOGW(TAG, "fetch failed sym=%s status=%d", s_list[idx].symbol, status);
        return;
    }

    float price = 0.0f, chg = 0.0f;
    if (parse_quote(buf, &price, &chg)) {
        s_list[idx].price = price;
        s_list[idx].change_pct = chg;
        s_list[idx].last_update_ms = t;
        s_list[idx].valid = true;
        ESP_LOGI(TAG, "sym=%s px=%.4f chg=%.2f%%", s_list[idx].symbol, price, chg);
    } else {
        ESP_LOGW(TAG, "parse failed sym=%s", s_list[idx].symbol);
    }
}

int Stocks_Count(void)
{
    return s_count;
}

bool Stocks_Get(int index, StockQuote* out)
{
    if (!out || !s_list) return false;
    if (index < 0 || index >= s_count) return false;
    *out = s_list[index];
    return out->valid;
}

