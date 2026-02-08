#pragma once
#include <stdint.h>

void St7735_Init(void);
void St7735_Fill(uint16_t color565);
void St7735_DrawPixel(int x, int y, uint16_t color565);

void St7735_BlitRect(int x, int y, int w, int h, const uint16_t* pixels565);
void St7735_FillRect(int x, int y, int w, int h, uint16_t color565);

int St7735_Width(void);
int St7735_Height(void);

void St7735_Flush(void);
