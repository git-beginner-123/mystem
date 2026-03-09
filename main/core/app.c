#include "core/app.h"
#include "core/app_state.h"
#include "core/app_events.h"

#include "ui/ui.h"
#include "input/input.h"
#include "input/drv_input_gpio_keys.h"
#include "drivers/led_guard.h"

#include "experiments/experiments_registry.h"
#include "experiments/experiment.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char* TAG = "APP_MEM";

static void log_mem_point(const char* phase, const Experiment* exp)
{
    const char* title = (exp && exp->title) ? exp->title : "(none)";
    size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t min8 = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "%s exp=%s free8=%u min8=%u free_int=%u",
             phase, title,
             (unsigned)free8, (unsigned)min8, (unsigned)free_internal);
}


static int main_menu_col_major_pos_from_index(int index, int count)
{
    int left_count = (count + 1) / 2; // even indices
    if ((index & 1) == 0) {
        return index / 2;
    }
    return left_count + (index / 2);
}

static int main_menu_index_from_col_major_pos(int pos, int count)
{
    int left_count = (count + 1) / 2; // even indices
    if (pos < left_count) {
        return pos * 2;
    }
    return (pos - left_count) * 2 + 1;
}

static int main_menu_move_vertical(int index, int count, int delta)
{
    if (count <= 1) return 0;
    int pos = main_menu_col_major_pos_from_index(index, count);
    pos = (pos + delta + count) % count;
    return main_menu_index_from_col_major_pos(pos, count);
}

static int main_menu_move_horizontal(int index, int count, int dir)
{
    // dir: -1 left, +1 right
    if (count <= 1) return 0;

    if (dir < 0) {
        if ((index & 1) == 1) return index - 1; // right -> left
        return index; // already left column
    }

    // left -> right when pair exists
    if ((index & 1) == 0 && (index + 1) < count) return index + 1;
    return index;
}

void App_Run(void)
{
    AppState st;
    AppState_Init(&st);

    Ui_Init();
    Input_Init();
    DrvInputGpioKeys_Init();
    LedGuard_AllOff();

    ExperimentContext ctx = (ExperimentContext){0};

    st.page = kPageMainMenu;
    st.main_index = 0;
    st.selected_exp_id = -1;
    st.desc_scroll = 0;

    Ui_DrawMainMenu(st.main_index, Experiments_Count());

    TickType_t last_gpio_poll = xTaskGetTickCount();
    bool last_versus_mode = false;

    while (1) {
        bool versus_mode = (st.page == kPageExperimentRun || st.page == kPageMazeRun);
        if (versus_mode != last_versus_mode) {
            DrvInputGpioKeys_SetVersusMode(versus_mode);
            if (!versus_mode) {
                LedGuard_AllOff();
            }
            last_versus_mode = versus_mode;
        }

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
        ctx.consume_back = false;

        // SFX policy:
        // No generic key-click sound. Sound is only triggered by explicit events
        // inside specific experiments.

        // -----------------------------
        // Main menu
        // -----------------------------
        if (st.page == kPageMainMenu) {
            if (ev.key == kInputUp) {
                st.main_index = main_menu_move_vertical(st.main_index, Experiments_Count(), -1);
                Ui_DrawMainMenu(st.main_index, Experiments_Count());
            }
            else if (ev.key == kInputDown) {
                st.main_index = main_menu_move_vertical(st.main_index, Experiments_Count(), +1);
                Ui_DrawMainMenu(st.main_index, Experiments_Count());
            }
            else if (ev.key == kInputLeft) {
                st.main_index = main_menu_move_horizontal(st.main_index, Experiments_Count(), -1);
                Ui_DrawMainMenu(st.main_index, Experiments_Count());
            }
            else if (ev.key == kInputRight) {
                st.main_index = main_menu_move_horizontal(st.main_index, Experiments_Count(), +1);
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
                    LedGuard_AllOff();
                    st.page = kPageMazeRun;
                    Ui_DrawMazeFullScreen();
                    log_mem_point("before_start", exp);
                    if (exp->start) exp->start(&ctx);
                    log_mem_point("after_start", exp);
                } else {
                    LedGuard_AllOff();
                    st.page = kPageExperimentRun;
                    Ui_DrawExperimentRun(exp->title);
                    log_mem_point("before_start", exp);
                    if (exp->start) exp->start(&ctx);
                    log_mem_point("after_start", exp);
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
                if (st.selected_exp_id == 23 && exp->on_key) {
                    exp->on_key(&ctx, ev.key);
                    if (ctx.consume_back) {
                        vTaskDelay(pdMS_TO_TICKS(1));
                        continue;
                    }
                }
                log_mem_point("before_stop", exp);
                if (exp->stop) exp->stop(&ctx);
                log_mem_point("after_stop", exp);
                if (exp->on_exit) exp->on_exit(&ctx);
                st.page = kPageMainMenu;
                Ui_DrawMainMenu(st.main_index, Experiments_Count());
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
                log_mem_point("before_stop", exp);
                if (exp->stop) exp->stop(&ctx);
                log_mem_point("after_stop", exp);
                if (exp->on_exit) exp->on_exit(&ctx);
                st.page = kPageMainMenu;
                Ui_DrawMainMenu(st.main_index, Experiments_Count());
            } else {
                if (exp->on_key) exp->on_key(&ctx, ev.key);
            }
        }

        // Keep animation progressing even when input events are frequent.
        if (st.page == kPageExperimentRun || st.page == kPageMazeRun) {
            const Experiment* exp = Experiments_GetById(st.selected_exp_id);
            if (exp && exp->tick) exp->tick(&ctx);
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
