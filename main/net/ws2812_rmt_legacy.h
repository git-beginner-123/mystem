#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool Ws2812_Init(void);
void Ws2812_SetRgb(uint8_t r, uint8_t g, uint8_t b);
void Ws2812_Off(void);

#ifdef __cplusplus
}
#endif
