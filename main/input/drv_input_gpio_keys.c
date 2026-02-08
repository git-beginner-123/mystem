#include "drv_input_gpio_keys.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "input/uart1_router.h"



//#include "drv_input.h"   // your existing action dispatcher

#define KEY_BACK_GPIO   38
#define KEY_DOWN_GPIO   39
#define KEY_OK_GPIO     0

#define KEY_ACTIVE_LEVEL 0
#define DEBOUNCE_MS     30

typedef struct {
    gpio_num_t gpio;
    int key;
    int last_level;
    int stable_level;
    int64_t last_change_us;
} key_t;


static key_t s_keys[] = {
    { KEY_BACK_GPIO,   kInputBack,    1, 1, 0 },
    { KEY_DOWN_GPIO, kInputDown,  1, 1, 0 },
    { KEY_OK_GPIO,   kInputEnter, 1, 1, 0 },
};


static void key_gpio_init(gpio_num_t gpio)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

void DrvInputGpioKeys_Init(void)
{
    key_gpio_init(KEY_BACK_GPIO);
    key_gpio_init(KEY_DOWN_GPIO);
    key_gpio_init(KEY_OK_GPIO);
}

void DrvInputGpioKeys_Poll(void)
{
    int64_t now = esp_timer_get_time();

    for (int i = 0; i < (int)(sizeof(s_keys)/sizeof(s_keys[0])); i++) {
        key_t* k = &s_keys[i];
        int level = gpio_get_level(k->gpio);

        if (level != k->last_level) {
            k->last_level = level;
            k->last_change_us = now;
        }

        if ((now - k->last_change_us) / 1000 >= DEBOUNCE_MS) {
            if (level != k->stable_level) {
                k->stable_level = level;

                if (level == KEY_ACTIVE_LEVEL) {
                    Uart1Router_InjectKey(k->key);
                }
            }
        }
    }
}
