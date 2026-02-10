#pragma once
#include <stdint.h>
#include "experiments/experiment.h"   

void Ui_Init(void);
void Ui_Clear(void);
void Ui_Println(const char* s);
void Ui_Printf(const char* fmt, ...);
void Ui_DrawMainMenu(int index, int count);

void Ui_DrawExperimentMenu(const char* title, const Experiment* exp, int scroll_line);
void Ui_DrawExperimentRun(const char* title);

void Ui_DrawMazeFullScreen(void);

void Ui_LcdLock(void);
void Ui_LcdUnlock(void);

void Ui_DrawFrame(const char* header_title, const char* footer_hint);
void Ui_DrawBodyClear(void);
void Ui_DrawBodyTextRowColor(int row, const char* text, uint16_t fg);
void Ui_DrawBodyTextRowTwoColor(int row, const char* left, const char* right, uint16_t left_fg, uint16_t right_fg);
uint16_t Ui_ColorRGB(uint8_t r, uint8_t g, uint8_t b);
void Ui_DrawTextAt(int x, int y, const char* text, uint16_t fg);
void Ui_DrawTextAtBg(int x, int y, const char* text, uint16_t fg, uint16_t bg);
void Ui_DrawGpioBody(int selected, bool red_on, bool green_on, bool yellow_on);
void Ui_DrawPwmBody(int selected, int red_pct, int green_pct, int yellow_pct, int freq_hz);
void Ui_DrawMicBody(const int* bands, int band_count, int freq_hz, int vol_pct);
void Ui_DrawSpeakerBody(bool playing, int vol_pct);
void Ui_DrawColorTestBody(int selected, bool sw_invert, bool sw_rb_swap, bool hw_invert);
