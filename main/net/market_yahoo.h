#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char symbol[16];
    float price;
    float change_pct;
    uint32_t last_update_ms;
    bool valid;
} MarketQuote;

void Markets_Init(const char* const* symbols, int count);
void Markets_Tick(uint32_t now_ms);

int  Markets_Count(void);
bool Markets_Get(int index, MarketQuote* out);
