#include "ui_console.h"
#include <string.h>

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void UiConsole_Init(UiConsole* c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->follow = true;
}

void UiConsole_Clear(UiConsole* c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->follow = true;
}

static int ring_oldest(const UiConsole* c)
{
    int oldest = c->head - c->count;
    while (oldest < 0) oldest += UI_CONSOLE_MAX_LINES;
    return oldest;
}

static int ring_index(const UiConsole* c, int index_from_oldest)
{
    int idx = ring_oldest(c) + index_from_oldest;
    idx %= UI_CONSOLE_MAX_LINES;
    return idx;
}

const char* UiConsole_GetLine(const UiConsole* c, int index_from_oldest)
{
    if (!c) return "";
    if (index_from_oldest < 0 || index_from_oldest >= c->count) return "";
    return c->lines[ring_index(c, index_from_oldest)];
}

int UiConsole_Count(const UiConsole* c)
{
    return c ? c->count : 0;
}

static void push_line(UiConsole* c, const char* s)
{
    if (!c) return;

    strncpy(c->lines[c->head], s ? s : "", UI_CONSOLE_LINE_CAP - 1);
    c->lines[c->head][UI_CONSOLE_LINE_CAP - 1] = 0;

    c->head = (c->head + 1) % UI_CONSOLE_MAX_LINES;
    if (c->count < UI_CONSOLE_MAX_LINES) c->count++;

    if (c->follow) {
        // Keep pinned; first will be computed in draw using visible_rows
        c->first = c->count > 0 ? c->count - 1 : 0;
    }
}

// Simple wrap: prefer spaces, fallback hard cut, supports '\n'
static const char* wrap_next(const char* p, int cols, char* out, int cap)
{
    if (!p) p = "";
    if (cols < 1) cols = 1;
    if (cap < 2) { if (cap == 1) out[0] = 0; return p; }

    while (*p == ' ' || *p == '\t') p++;

    if (*p == '\n') {
        out[0] = 0;
        return p + 1;
    }

    int maxc = cols;
    if (maxc > cap - 1) maxc = cap - 1;

    int n = 0;
    int last_sp_out = -1;
    const char* last_sp_p = NULL;

    while (*p && *p != '\n' && n < maxc) {
        char ch = *p;
        out[n++] = ch;
        if (ch == ' ' || ch == '\t') {
            last_sp_out = n - 1;
            last_sp_p = p;
        }
        p++;
    }

    if (*p == '\n') {
        out[n] = 0;
        return p + 1;
    }

    if (n == maxc && *p && *p != '\n' && last_sp_out >= 0) {
        int cut = last_sp_out;
        while (cut > 0 && (out[cut] == ' ' || out[cut] == '\t')) cut--;
        out[cut + 1] = 0;

        p = last_sp_p + 1;
        while (*p == ' ' || *p == '\t') p++;
        return p;
    }

    out[n] = 0;
    return p;
}

void UiConsole_AppendWrapped(UiConsole* c, const char* text, int cols)
{
    if (!c) return;
    if (!text) text = "";

    const char* p = text;
    while (*p) {
        char line[UI_CONSOLE_LINE_CAP];
        p = wrap_next(p, cols, line, (int)sizeof(line));
        push_line(c, line);
    }
}

void UiConsole_ScrollOlder(UiConsole* c, int visible_rows)
{
    if (!c) return;
    if (visible_rows < 1) visible_rows = 1;

    int max_first = c->count - visible_rows;
    if (max_first < 0) max_first = 0;

    // If following tail, compute current first as last page
    int first = c->follow ? max_first : c->first;

    first -= 1;
    first = clamp_int(first, 0, max_first);

    c->first = first;
    c->follow = (c->first >= max_first);
}

void UiConsole_ScrollNewer(UiConsole* c, int visible_rows)
{
    if (!c) return;
    if (visible_rows < 1) visible_rows = 1;

    int max_first = c->count - visible_rows;
    if (max_first < 0) max_first = 0;

    int first = c->follow ? max_first : c->first;

    first += 1;
    first = clamp_int(first, 0, max_first);

    c->first = first;
    c->follow = (c->first >= max_first);
}