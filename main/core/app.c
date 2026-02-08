#include "core/app.h"
#include "core/app_state.h"
#include "core/app_events.h"

#include "ui/ui.h"
#include "input/input.h"
#include "input/drv_input_gpio_keys.h"

#include "experiments/experiments_registry.h"
#include "experiments/experiment.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static void clamp_wrap(int* value, int minv, int maxv)
{
    if (*value < minv) *value = maxv;
    if (*value > maxv) *value = minv;
}

void App_Run(void)
{
    AppState st;
    AppState_Init(&st);

    Ui_Init();
    Input_Init();
    DrvInputGpioKeys_Init();

    ExperimentContext ctx = (ExperimentContext){0};

    st.page = kPageMainMenu;
    st.main_index = 0;
    st.selected_exp_id = -1;
    st.desc_scroll = 0;

    Ui_DrawMainMenu(st.main_index, Experiments_Count());

    TickType_t last_gpio_poll = xTaskGetTickCount();

    while (1) {
        TickType_t now = xTaskGetTickCount();
        if ((now - last_gpio_poll) >= pdMS_TO_TICKS(10)) {
            last_gpio_poll = now;
            DrvInputGpioKeys_Poll();
        }

        AppEvent ev;
        if (!AppEvents_Poll(&ev, 50)) {
            if (st.page == kPageExperimentRun || st.page == kPageMazeRun) {
                const Experiment* exp = Experiments_GetById(st.selected_exp_id);
                if (exp && exp->tick) exp->tick(&ctx);
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        // -----------------------------
        // Main menu
        // -----------------------------
        if (st.page == kPageMainMenu) {
            if (ev.key == kInputUp) {
                st.main_index--;
                clamp_wrap(&st.main_index, 0, Experiments_Count() - 1);
                Ui_DrawMainMenu(st.main_index, Experiments_Count());
            }
            else if (ev.key == kInputDown) {
                st.main_index++;
                clamp_wrap(&st.main_index, 0, Experiments_Count() - 1);
                Ui_DrawMainMenu(st.main_index, Experiments_Count());
            }
            else if (ev.key == kInputEnter) {
                const Experiment* exp = Experiments_GetByIndex(st.main_index);
                if (exp) {
                    st.selected_exp_id = exp->id;
                    st.desc_scroll = 0;
                    st.page = kPageExperimentMenu; // description page

                    if (exp->on_enter) exp->on_enter(&ctx);

                    Ui_DrawExperimentMenu(exp->title, exp, st.desc_scroll);
                }
            }
        }

        // -----------------------------
        // Description page (2nd level)
        // -----------------------------
        else if (st.page == kPageExperimentMenu) {
            const Experiment* exp = Experiments_GetById(st.selected_exp_id);
            if (!exp) {
                st.page = kPageMainMenu;
                Ui_DrawMainMenu(st.main_index, Experiments_Count());
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            if (ev.key == kInputUp) {
                st.desc_scroll--;
                if (st.desc_scroll < 0) st.desc_scroll = 0;
                Ui_DrawExperimentMenu(exp->title, exp, st.desc_scroll);
            }
            else if (ev.key == kInputDown) {
                st.desc_scroll++;
                Ui_DrawExperimentMenu(exp->title, exp, st.desc_scroll);
            }
            else if (ev.key == kInputBack) {
                if (exp->on_exit) exp->on_exit(&ctx);
                st.page = kPageMainMenu;
                Ui_DrawMainMenu(st.main_index, Experiments_Count());
            }
            else if (ev.key == kInputEnter) {
                // Maze special case: full screen
                if (exp->id == 12) {   // TODO: replace with your real maze id
                    st.page = kPageMazeRun;
                    Ui_DrawMazeFullScreen();
                    if (exp->start) exp->start(&ctx);
                } else {
                    st.page = kPageExperimentRun;
                    Ui_DrawExperimentRun(exp->title);
                    if (exp->start) exp->start(&ctx);
                }
            }
        }

        // -----------------------------
        // Run page (3rd level)
        // -----------------------------
        else if (st.page == kPageExperimentRun) {
            const Experiment* exp = Experiments_GetById(st.selected_exp_id);
            if (!exp) {
                st.page = kPageMainMenu;
                Ui_DrawMainMenu(st.main_index, Experiments_Count());
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            if (ev.key == kInputBack) {
                if (exp->stop) exp->stop(&ctx);
                st.page = kPageExperimentMenu;
                Ui_DrawExperimentMenu(exp->title, exp, st.desc_scroll);
            } else {
                if (exp->on_key) exp->on_key(&ctx, ev.key);
            }
        }

        // -----------------------------
        // Maze full screen page
        // -----------------------------
        else if (st.page == kPageMazeRun) {
            const Experiment* exp = Experiments_GetById(st.selected_exp_id);
            if (!exp) {
                st.page = kPageMainMenu;
                Ui_DrawMainMenu(st.main_index, Experiments_Count());
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            if (ev.key == kInputBack) {
                if (exp->stop) exp->stop(&ctx);
                st.page = kPageExperimentMenu;
                Ui_DrawExperimentMenu(exp->title, exp, st.desc_scroll);
            } else {
                if (exp->on_key) exp->on_key(&ctx, ev.key);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}