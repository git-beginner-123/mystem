#include "experiments/experiment.h"
#include "ui/ui.h"
#include "net/remote_web.h"
#include "comm_wifi.h"
#include "display/st7735.h"
#include <string.h>
#include <stdio.h>

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("WIFI AP", "BACK=RET");
    Ui_DrawBodyClear();
    Ui_DrawBodyTextRowColor(0, "SSID: ESP32_RC", Ui_ColorRGB(180, 220, 255));
    Ui_DrawBodyTextRowColor(1, "IP: 192.168.4.1", Ui_ColorRGB(180, 220, 255));
    Ui_DrawBodyTextRowColor(2, "HTTP: /", Ui_ColorRGB(200, 200, 200));
}

static bool s_display_on = true;
static bool s_screen_drawn = false;
static char s_last_cmd[16];

static void draw_screen(void)
{
    Ui_DrawFrame("WIFI AP", "BACK=RET");
    Ui_DrawBodyClear();
    Ui_DrawBodyTextRowColor(0, "SSID: ESP32_RC", Ui_ColorRGB(180, 220, 255));
    Ui_DrawBodyTextRowColor(1, "IP: 192.168.4.1", Ui_ColorRGB(180, 220, 255));
    Ui_DrawBodyTextRowColor(2, "STATUS: RUN", Ui_ColorRGB(200, 200, 200));
    char line[32];
    snprintf(line, sizeof(line), "CMD: %s", s_last_cmd[0] ? s_last_cmd : "NONE");
    Ui_DrawBodyTextRowColor(3, line, Ui_ColorRGB(200, 200, 200));
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    comm_wifi_stop();
    s_display_on = true;
    s_screen_drawn = false;
    s_last_cmd[0] = 0;

    Ui_DrawFrame("WIFI AP", "BACK=RET");
    Ui_DrawBodyClear();
    Ui_DrawBodyTextRowColor(0, "Starting...", Ui_ColorRGB(200, 200, 200));
    RemoteWeb_Start();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    RemoteWeb_Stop();
    Ui_DrawFrame("WIFI AP", "BACK=RET");
    Ui_DrawBodyClear();
    Ui_DrawBodyTextRowColor(0, "Stopped", Ui_ColorRGB(200, 200, 200));
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;

    RemoteCmd cmd = RemoteWeb_PopCmd();
    if (cmd != kRemoteCmdNone) {
        if (cmd == kRemoteCmdUp) strcpy(s_last_cmd, "UP");
        if (cmd == kRemoteCmdDown) strcpy(s_last_cmd, "DOWN");
        if (cmd == kRemoteCmdLeft) strcpy(s_last_cmd, "LEFT");
        if (cmd == kRemoteCmdRight) strcpy(s_last_cmd, "RIGHT");
        if (cmd == kRemoteCmdStart) strcpy(s_last_cmd, "START");
        if (cmd == kRemoteCmdStop) strcpy(s_last_cmd, "STOP");
    }

    bool display_on = RemoteWeb_DisplayOn();
    if (!display_on && s_display_on) {
        s_display_on = false;
        St7735_Fill(0x0000);
        St7735_Flush();
        return;
    }

    if (display_on && !s_display_on) {
        s_display_on = true;
        s_screen_drawn = false;
    }

    if (!s_display_on) return;
    if (!s_screen_drawn) {
        draw_screen();
        s_screen_drawn = true;
        return;
    }

    if (cmd != kRemoteCmdNone) {
        draw_screen();
    }
}

const Experiment g_exp_wifi_ap = {
    .id = 9,
    .title = "WIFI AP",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = 0,
    .tick = tick,
};
