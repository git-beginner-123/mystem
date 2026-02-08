#include "experiments/experiment.h"
#include "ui/ui.h"

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_Clear();
    Ui_Println("SPK");
    Ui_Println("GOAL: TBD");
    Ui_Println("HW:   TBD");
    Ui_Println("OBS:  TBD");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_Clear();
    Ui_Println("MIC");
    Ui_Println("RUNNING...");
}

const Experiment g_exp_speaker = {
    .id = 7,
    .title = "SPK",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = 0,
    .on_key = 0,
    .tick = 0,
};
