#include "experiments/experiment.h"
#include "ui/ui.h"
#include "net/remote_web.h"

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_Clear();
    Ui_Println("WIFI AP REMOTE");
    Ui_Println("GOAL: Web LED control");
    Ui_Println("SSID: STEM-REMOTE");
    Ui_Println("IP:   192.168.4.1");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_Clear();
    Ui_Println("WIFI AP REMOTE");
    Ui_Println("Starting...");
    RemoteWeb_Start();

    Ui_Clear();
    Ui_Println("WIFI AP REMOTE");
    Ui_Println("SSID: STEM-REMOTE");
    Ui_Println("IP:   192.168.4.1");
    Ui_Println("Open: http://192.168.4.1/");
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    RemoteWeb_Stop();
    Ui_Clear();
    Ui_Println("WIFI AP REMOTE");
    Ui_Println("Stopped");
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
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
