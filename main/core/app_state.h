#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    kPageMainMenu = 0,
    kPageExperimentMenu,
    kPageExperimentRun,
    kPageMazeRun,          // <-- add this
} AppPage;

typedef struct {
    AppPage page;
    int main_index;
    int selected_exp_id;

    int desc_scroll;       // <-- add this

    // remove exp_menu_index if no longer used
} AppState;

void AppState_Init(AppState* s);
