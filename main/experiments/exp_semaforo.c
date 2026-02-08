#include "experiments/experiment.h"
#include "ui/ui.h"

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_Clear();
    Ui_Println("SMF");
    Ui_Println("GOAL: TBD");
    Ui_Println("HW:   TBD");
    Ui_Println("OBS:  TBD");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_Clear();
    Ui_Println("SMF");
    Ui_Println("RUNNING...");
}

const Experiment g_exp_semaforo = {
    .id = 11,
    .title = "SEMAFORO",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = 0,
    .on_key = 0,
    .tick = 0,
};
