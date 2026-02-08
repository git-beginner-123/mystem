#include "electronic_menu.h"

// This module is only a wrapper around your existing "10 experiments" menu.

typedef struct {
    int selected;
    bool exit_requested;
} ElectronicMenuState;

static ElectronicMenuState s_em = {0};

void ElectronicMenu_Init(void)
{
    s_em.selected = 0;
    s_em.exit_requested = false;
}

bool ElectronicMenu_IsExitRequested(void)
{
    return s_em.exit_requested;
}

void ElectronicMenu_ClearExitRequested(void)
{
    s_em.exit_requested = false;
}

void ElectronicMenu_OnAction(Action act)
{
    if (act == kActionUp) {
        if (s_em.selected > 0) s_em.selected--;
    } else if (act == kActionDown) {
        if (s_em.selected < 9) s_em.selected++;
    } else if (act == kActionOk) {
        // Call the selected experiment entry here.
        // Example:
        // Electronic_RunExperiment(s_em.selected);
    } else if (act == kActionBack) {
        s_em.exit_requested = true;
    }
}

static void DrawLine(int index, const char* text, bool selected)
{
    int y = UI_TOP + index * UI_LINE_H;

    if (selected) {
        LCD_FillRect(0, y - 2, LCD_W, UI_LINE_H, COLOR_BLUE);
        LCD_DrawText(UI_LEFT, y, text, COLOR_WHITE, COLOR_BLUE);
        LCD_DrawChar(2, y, '>', COLOR_WHITE, COLOR_BLUE); // 光标
    } else {
        LCD_DrawText(UI_LEFT, y, text, COLOR_BLACK, COLOR_WHITE);
    }
}

static void DrawHeader(void)
{
    LCD_FillRect(0, 0, LCD_W, 18, COLOR_DARKBLUE);
    LCD_DrawText(6, 2, "Electronic Lab", COLOR_WHITE, COLOR_DARKBLUE);
}
static void DrawFooter(void)
{
    LCD_FillRect(0, LCD_H - 16, LCD_W, 16, COLOR_LIGHTGRAY);
    LCD_DrawText(6, LCD_H - 14, "UP/DN Select  OK Enter", COLOR_BLACK, COLOR_LIGHTGRAY);
}
void ElectronicMenu_Render(void)
{
    LCD_Clear(COLOR_WHITE);

    DrawHeader();

    const char* items[] = {
        "Experiment 01",
        "Experiment 02",
        "Experiment 03",
        "Experiment 04",
        "Experiment 05",
        "Experiment 06",
        "Experiment 07",
        "Experiment 08",
        "Experiment 09",
        "Experiment 10",
        "Experiment 11",
    };

    for (int i = 0; i < 11; i++) {
        DrawLine(i, items[i], s_em.selected == i);
    }

    DrawFooter();
}11