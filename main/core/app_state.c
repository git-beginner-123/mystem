#include "core/app_state.h"


void AppState_Init(AppState* s)
{
    if (!s) return;

    s->page = kPageMainMenu;

    s->main_index = 0;
    s->selected_exp_id = -1;

    s->desc_scroll = 0;
}