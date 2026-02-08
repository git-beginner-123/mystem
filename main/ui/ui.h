#pragma once
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
void Ui_DrawGpioBody(int selected, bool red_on, bool green_on, bool yellow_on);
