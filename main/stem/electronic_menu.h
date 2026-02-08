#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    kActionNone = 0,
    kActionUp,
    kActionDown,
    kActionOk,
    kActionBack,
} Action;

void ElectronicMenu_Init(void);
void ElectronicMenu_OnAction(Action act);
void ElectronicMenu_Render(void);
bool ElectronicMenu_IsExitRequested(void);
void ElectronicMenu_ClearExitRequested(void);
