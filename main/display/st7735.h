#pragma once
#include <stdint.h>
#include <stdbool.h>

void St7735_Init(void);
void St7735_Fill(uint16_t color565);
void St7735_DrawPixel(int x, int y, uint16_t color565);

void St7735_BlitRect(int x, int y, int w, int h, const uint16_t* pixels565);
void St7735_FillRect(int x, int y, int w, int h, uint16_t color565);

int St7735_Width(void);
int St7735_Height(void);

void St7735_Flush(void);

// Panel control helpers
void St7735_SetInversion(bool on);
bool St7735_GetInversion(void);

// Software color correction (applied to all pixels before sending)
void St7735_SetSoftwareInvert(bool on);
void St7735_SetSoftwareRBSwap(bool on);
bool St7735_GetSoftwareInvert(void);
bool St7735_GetSoftwareRBSwap(void);
