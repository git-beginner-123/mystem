#include "experiments/experiments_registry.h"

extern const Experiment g_exp_gpio;
extern const Experiment g_exp_pwm;
extern const Experiment g_exp_adc;
extern const Experiment g_exp_uart;
extern const Experiment g_exp_tof;
extern const Experiment g_exp_mic;
extern const Experiment g_exp_speaker;
extern const Experiment g_exp_ble;
extern const Experiment g_exp_wifi_ap;
extern const Experiment g_exp_wifi_sta;
extern const Experiment g_exp_semaforo;
extern const Experiment g_exp_maze;
extern const Experiment g_exp_lcd_color;

static const Experiment* kList[] = {
    &g_exp_gpio,
    &g_exp_pwm,
    &g_exp_adc,
    &g_exp_uart,
    &g_exp_tof,
    &g_exp_mic,
    &g_exp_speaker,
    &g_exp_ble,
    &g_exp_wifi_ap,
    &g_exp_wifi_sta,
    &g_exp_semaforo,
    &g_exp_maze,
    &g_exp_lcd_color,
};

int Experiments_Count(void)
{
    return (int)(sizeof(kList) / sizeof(kList[0]));
}

const Experiment* Experiments_GetByIndex(int index)
{
    if (index < 0 || index >= Experiments_Count()) return 0;
    return kList[index];
}

const Experiment* Experiments_GetById(int id)
{
    for (int i = 0; i < Experiments_Count(); i++) {
        if (kList[i]->id == id) return kList[i];
    }
    return 0;
}
