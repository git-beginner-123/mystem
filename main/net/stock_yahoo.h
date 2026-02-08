#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char symbol[12];
    float price;
    float change_pct;
    uint32_t last_update_ms;
    bool valid;
} StockQuote;

void Stocks_Init(const char* const* symbols, int count);
void Stocks_Tick(uint32_t now_ms);

int  Stocks_Count(void);
bool Stocks_Get(int index, StockQuote* out);

#ifdef __cplusplus
}
#endif
