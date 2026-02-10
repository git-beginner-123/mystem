#include "market_yahoo.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "markets";

static MarketQuote* s_list = NULL;
static int s_count = 0;

static uint32_t s_next_fetch_ms = 0;
static int s_next_index = 0;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static int http_get_all(const char* url, char** io_buf, int* io_cap, int* out_status)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;

    esp_http_client_set_header(c, "Accept-Encoding", "identity");
    esp_http_client_set_header(c, "User-Agent", "Mozilla/5.0");

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return -1;
    }

    int hdr_len = esp_http_client_fetch_headers(c);
    if (hdr_len < 0) {
        ESP_LOGW(TAG, "fetch headers failed: len=%d", hdr_len);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return -1;
    }

    int status = esp_http_client_get_status_code(c);
    if (out_status) *out_status = status;

    int content_len = esp_http_client_get_content_length(c);
    if (content_len > 0) {
        int need = content_len + 1;
        if (!*io_buf || !*io_cap || *io_cap < need) {
            char* nb = (char*)realloc(*io_buf, (size_t)need);
            if (!nb) {
                esp_http_client_close(c);
                esp_http_client_cleanup(c);
                return -1;
            }
            *io_buf = nb;
            *io_cap = need;
        }
    } else {
        if (!*io_buf || !*io_cap) {
            *io_cap = 8 * 1024;
            *io_buf = (char*)malloc((size_t)*io_cap);
            if (!*io_buf) {
                esp_http_client_close(c);
                esp_http_client_cleanup(c);
                return -1;
            }
        }
    }

    int total = 0;
    int empty_reads = 0;
    uint32_t start_ms = now_ms();
    const uint32_t max_wait_ms = 12000;
    while (!esp_http_client_is_complete_data_received(c)) {
        int room = (*io_cap) - 1 - total;
        if (room <= 0) {
            int new_cap = (*io_cap) * 2;
            char* nb = (char*)realloc(*io_buf, (size_t)new_cap);
            if (!nb) break;
            *io_buf = nb;
            *io_cap = new_cap;
            room = (*io_cap) - 1 - total;
        }

        int n = esp_http_client_read(c, (*io_buf) + total, room);
        if (n > 0) {
            total += n;
            empty_reads = 0;
            continue;
        }

        if (n == 0) {
            empty_reads++;
            if ((now_ms() - start_ms) > max_wait_ms) break;
            if (empty_reads > 20) vTaskDelay(pdMS_TO_TICKS(50));
            else vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // n < 0
        break;
    }
    (*io_buf)[total] = 0;

    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return total;
}

static bool parse_quote(const char* json, float* out_price, float* out_chg_pct)
{
    // Fast parse from the meta section (appears early in the payload).
    const char* kPrice = "\"regularMarketPrice\":";
    const char* kChg = "\"regularMarketChangePercent\":";
    const char* kPrev = "\"regularMarketPreviousClose\":";
    const char* kPrev2 = "\"chartPreviousClose\":";

    const char* p = strstr(json, kPrice);
    const char* c = strstr(json, kChg);
    const char* pv = strstr(json, kPrev);
    const char* pv2 = strstr(json, kPrev2);

    if (p) {
        p += strlen(kPrice);
        char* endp = NULL;
        double price = strtod(p, &endp);
        if (endp != p) {
            if (c) {
                c += strlen(kChg);
                char* endc = NULL;
                double chg = strtod(c, &endc);
                if (endc != c) {
                    *out_price = (float)price;
                    *out_chg_pct = (float)chg;
                    return true;
                }
            }
            if (pv || pv2) {
                const char* pp = pv ? pv : pv2;
                const char* key = pv ? kPrev : kPrev2;
                pp += strlen(key);
                char* endv = NULL;
                double prev = strtod(pp, &endv);
                if (endv != pp && prev > 0.0) {
                    *out_price = (float)price;
                    *out_chg_pct = (float)(((price - prev) / prev) * 100.0);
                    return true;
                }
            }
        }
    }

    cJSON* root = cJSON_Parse(json);
    if (!root) return false;

    bool ok = false;

    cJSON* chart  = cJSON_GetObjectItem(root, "chart");
    cJSON* result = chart ? cJSON_GetObjectItem(chart, "result") : NULL;
    cJSON* r0     = (result && cJSON_IsArray(result)) ? cJSON_GetArrayItem(result, 0) : NULL;
    cJSON* meta   = r0 ? cJSON_GetObjectItem(r0, "meta") : NULL;

    cJSON* price  = meta ? cJSON_GetObjectItem(meta, "regularMarketPrice") : NULL;
    cJSON* chg    = meta ? cJSON_GetObjectItem(meta, "regularMarketChangePercent") : NULL;
    cJSON* prev   = meta ? cJSON_GetObjectItem(meta, "regularMarketPreviousClose") : NULL;
    cJSON* prev2  = meta ? cJSON_GetObjectItem(meta, "chartPreviousClose") : NULL;

    if (cJSON_IsNumber(price)) {
        *out_price = (float)price->valuedouble;
        if (cJSON_IsNumber(chg)) {
            *out_chg_pct = (float)chg->valuedouble;
            ok = true;
        } else {
            cJSON* use_prev = cJSON_IsNumber(prev) ? prev : (cJSON_IsNumber(prev2) ? prev2 : NULL);
            if (use_prev && use_prev->valuedouble > 0.0) {
                double p0 = use_prev->valuedouble;
                *out_chg_pct = (float)(((*out_price - p0) / p0) * 100.0);
                ok = true;
            }
        }
    }

    cJSON_Delete(root);
    return ok;
}

void Markets_Init(const char* const* symbols, int count)
{
    if (s_list) {
        free(s_list);
        s_list = NULL;
    }
    s_count = 0;
    s_next_index = 0;
    s_next_fetch_ms = 0;

    if (!symbols || count <= 0) return;

    s_list = (MarketQuote*)calloc((size_t)count, sizeof(MarketQuote));
    if (!s_list) return;

    s_count = count;
    for (int i = 0; i < count; i++) {
        strncpy(s_list[i].symbol, symbols[i], sizeof(s_list[i].symbol) - 1);
        s_list[i].valid = false;
    }
}

void Markets_Tick(uint32_t unused_now_ms)
{
    (void)unused_now_ms;

    if (!s_list || s_count <= 0) return;

    uint32_t t = now_ms();
    if (t < s_next_fetch_ms) return;

    // Fetch all symbols each period
    s_next_fetch_ms = t + 30000; // 30s

    static char* buf = NULL;
    static int cap = 0;

    for (int idx = 0; idx < s_count; idx++) {
        char url[160];
        snprintf(url, sizeof(url),
                 "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1d&range=1d",
                 s_list[idx].symbol);

        int status = 0;
        int n = http_get_all(url, &buf, &cap, &status);
        if (n <= 0 || status != 200) {
            ESP_LOGW(TAG, "fetch failed sym=%s status=%d", s_list[idx].symbol, status);
            continue;
        }

        float price = 0.0f, chg = 0.0f;
        if (parse_quote(buf, &price, &chg)) {
            s_list[idx].price = price;
            s_list[idx].change_pct = chg;
            s_list[idx].last_update_ms = t;
            s_list[idx].valid = true;
        } else {
            ESP_LOGW(TAG, "parse failed sym=%s status=%d len=%d", s_list[idx].symbol, status, n);
        }
    }
}

int Markets_Count(void)
{
    return s_count;
}

bool Markets_Get(int index, MarketQuote* out)
{
    if (!out || !s_list) return false;
    if (index < 0 || index >= s_count) return false;
    *out = s_list[index];
    return out->valid;
}
