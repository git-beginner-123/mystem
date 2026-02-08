#pragma once
#include <stdbool.h>

#define UI_CONSOLE_MAX_LINES  64
#define UI_CONSOLE_LINE_CAP   64

typedef struct {
    char lines[UI_CONSOLE_MAX_LINES][UI_CONSOLE_LINE_CAP];
    int head;       // next write index (ring)
    int count;      // valid lines
    int first;      // first visible line index (from oldest)
    bool follow;    // follow tail when true
} UiConsole;

void UiConsole_Init(UiConsole* c);
void UiConsole_Clear(UiConsole* c);
void UiConsole_AppendWrapped(UiConsole* c, const char* text, int cols);
void UiConsole_ScrollOlder(UiConsole* c, int visible_rows);
void UiConsole_ScrollNewer(UiConsole* c, int visible_rows);
const char* UiConsole_GetLine(const UiConsole* c, int index_from_oldest);
int UiConsole_Count(const UiConsole* c);