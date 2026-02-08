#include <stdint.h>
#include <stdbool.h>

#include "display/st7735.h"
#include "ui/ui.h"
#include "experiments/experiment.h"

// -----------------------------
// InputKey mapping (edit these 4 macros)
// -----------------------------
#ifndef MAZE_KEY_UP
#define MAZE_KEY_UP    (0)
#endif
#ifndef MAZE_KEY_DOWN
#define MAZE_KEY_DOWN  (0)
#endif
#ifndef MAZE_KEY_LEFT
#define MAZE_KEY_LEFT  (0)
#endif
#ifndef MAZE_KEY_RIGHT
#define MAZE_KEY_RIGHT (0)
#endif

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

#define TILE_W 16
#define TILE_H 16

#define MAP_W 15
#define MAP_H 20

static const uint8_t kMap[MAP_H][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,1,0,0,0,0,0,0,0,1},
    {1,0,1,1,1,0,1,0,1,1,1,1,1,0,1},
    {1,0,1,0,0,0,0,0,0,0,0,0,1,0,1},
    {1,0,1,0,1,1,1,1,1,1,1,0,1,0,1},
    {1,0,0,0,1,0,0,0,0,0,1,0,0,0,1},
    {1,1,1,0,1,0,1,1,1,0,1,1,1,0,1},
    {1,0,0,0,0,0,1,0,0,0,0,0,1,0,1},
    {1,0,1,1,1,0,1,0,1,1,1,0,1,0,1},
    {1,0,0,0,1,0,0,0,1,0,0,0,0,0,1},
    {1,1,1,0,1,1,1,0,1,0,1,1,1,0,1},
    {1,0,0,0,0,0,0,0,1,0,0,0,1,0,1},
    {1,0,1,1,1,1,1,0,1,1,1,0,1,0,1},
    {1,0,0,0,0,0,1,0,0,0,1,0,0,0,1},
    {1,1,1,1,1,0,1,1,1,0,1,1,1,0,1},
    {1,0,0,0,1,0,0,0,1,0,0,0,0,0,1},
    {1,0,1,0,1,1,1,0,1,1,1,1,1,0,1},
    {1,0,1,0,0,0,0,0,0,0,0,0,1,0,1},
    {1,0,0,0,1,0,1,1,1,1,1,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

// -----------------------------
// State
// -----------------------------
static bool s_running = false;

static int s_px = 1;
static int s_py = 1;

static int s_prev_px = 1;
static int s_prev_py = 1;

static int s_ox = 0;
static int s_oy = 0;

static bool s_dirty = false;     // need redraw dirty tiles
static bool s_full_dirty = false; // need redraw full map

static uint16_t s_tilebuf[TILE_W * TILE_H];

// -----------------------------
// Tile drawing
// -----------------------------
static void tile_fill(uint16_t c)
{
    for (int i = 0; i < TILE_W * TILE_H; i++) s_tilebuf[i] = c;
}

static void tile_draw_wall(void)
{
    uint16_t bg = rgb565(10, 10, 20);
    uint16_t fg = rgb565(40, 160, 140);

    tile_fill(bg);

    for (int y = 0; y < TILE_H; y++) {
        for (int x = 0; x < TILE_W; x++) {
            if (y == 0 || y == TILE_H - 1 || x == 0 || x == TILE_W - 1) {
                s_tilebuf[y * TILE_W + x] = fg;
            } else if ((y % 4) == 0) {
                s_tilebuf[y * TILE_W + x] = fg;
            } else if ((x % 6) == 0 && ((y / 4) % 2 == 0)) {
                s_tilebuf[y * TILE_W + x] = fg;
            }
        }
    }
}

static void tile_draw_floor(void)
{
    uint16_t c0 = rgb565(8, 12, 18);
    uint16_t c1 = rgb565(10, 16, 26);

    for (int y = 0; y < TILE_H; y++) {
        for (int x = 0; x < TILE_W; x++) {
            bool chk = (((x >> 2) + (y >> 2)) & 1) != 0;
            s_tilebuf[y * TILE_W + x] = chk ? c0 : c1;
        }
    }
}

static void tile_draw_player_overlay(void)
{
    uint16_t p = rgb565(255, 255, 255);
    uint16_t a = rgb565(255, 220, 80);

    int cx = TILE_W / 2;
    int cy = TILE_H / 2;

    for (int y = 0; y < TILE_H; y++) {
        for (int x = 0; x < TILE_W; x++) {
            int dx = x - cx;
            int dy = y - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 <= 18) s_tilebuf[y * TILE_W + x] = a;
            if (d2 <= 8)  s_tilebuf[y * TILE_W + x] = p;
        }
    }
}

static int map_origin_x(void)
{
    int sw = St7735_Width();
    int mw = MAP_W * TILE_W;
    if (mw >= sw) return 0;
    return (sw - mw) / 2;
}

static int map_origin_y(void)
{
    int sh = St7735_Height();
    int mh = MAP_H * TILE_H;
    if (mh >= sh) return 0;
    return (sh - mh) / 2;
}

static void draw_tile(int mx, int my, bool player_here)
{
    if (kMap[my][mx] == 1) tile_draw_wall();
    else {
        tile_draw_floor();
        if (player_here) tile_draw_player_overlay();
    }

    int sx = s_ox + mx * TILE_W;
    int sy = s_oy + my * TILE_H;
    St7735_BlitRect(sx, sy, TILE_W, TILE_H, s_tilebuf);
}

static void redraw_full(void)
{
    for (int y = 0; y < MAP_H; y++) {
        for (int x = 0; x < MAP_W; x++) {
            bool p = (x == s_px && y == s_py);
            draw_tile(x, y, p);
        }
    }
    St7735_Flush();
}

static void redraw_dirty(void)
{
    // redraw old player tile and new player tile
    draw_tile(s_prev_px, s_prev_py, false);
    draw_tile(s_px, s_py, true);
    St7735_Flush();
}

// -----------------------------
// Movement (only update state, drawing happens in tick)
// -----------------------------
static void try_move(int dx, int dy)
{
    int nx = s_px + dx;
    int ny = s_py + dy;

    if (nx < 0 || nx >= MAP_W || ny < 0 || ny >= MAP_H) return;
    if (kMap[ny][nx] != 0) return;

    s_prev_px = s_px;
    s_prev_py = s_py;

    s_px = nx;
    s_py = ny;

    s_dirty = true;
}

// -----------------------------
// Experiment callbacks
// -----------------------------
static void Maze_OnEnter(ExperimentContext* ctx)
{
    (void)ctx;
    s_running = false;
}

static void Maze_OnExit(ExperimentContext* ctx)
{
    (void)ctx;
    s_running = false;
}

static void Maze_ShowRequirements(ExperimentContext* ctx)
{
    (void)ctx;

    Ui_Clear();
    Ui_Println("MAZE DEMO");
    Ui_Println("Move: UP/DN/LT/RT");
    Ui_Println("BACK: return");
    Ui_Println("");
    Ui_Println("Render in tick()");
    St7735_Flush();
}

static void Maze_Start(ExperimentContext* ctx)
{
    (void)ctx;

    s_running = true;

    s_ox = map_origin_x();
    s_oy = map_origin_y();

    s_px = 1;
    s_py = 1;
    s_prev_px = s_px;
    s_prev_py = s_py;

    // Mark for full redraw in tick to override any framework "RUNNING" repaint
    s_full_dirty = true;
    s_dirty = false;
}

static void Maze_Stop(ExperimentContext* ctx)
{
    (void)ctx;
    s_running = false;
}

static void Maze_OnKey(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (!s_running) return;

    if (key == MAZE_KEY_UP)    { try_move(0, -1); return; }
    if (key == MAZE_KEY_DOWN)  { try_move(0,  1); return; }
    if (key == MAZE_KEY_LEFT)  { try_move(-1, 0); return; }
    if (key == MAZE_KEY_RIGHT) { try_move(1,  0); return; }
}

static void Maze_Tick(ExperimentContext* ctx)
{
    (void)ctx;
    if (!s_running) return;

    if (s_full_dirty) {
        // Clear and draw the maze fully
        St7735_Fill(rgb565(0, 0, 0));
        St7735_Flush();
        redraw_full();
        s_full_dirty = false;
        return;
    }

    if (s_dirty) {
        redraw_dirty();
        s_dirty = false;
    }
}

const Experiment g_exp_maze = {
    .id = 12,
    .title = "MAZE",

    .on_enter = Maze_OnEnter,
    .on_exit = Maze_OnExit,

    .show_requirements = Maze_ShowRequirements,

    .start = Maze_Start,
    .stop = Maze_Stop,

    .on_key = Maze_OnKey,
    .tick = Maze_Tick,
};