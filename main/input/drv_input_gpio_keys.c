#include "drv_input_gpio_keys.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "input/uart1_router.h"

#define KEY_ACTIVE_LEVEL      0
#define KEY_DEBOUNCE_MS       25
#define KEY_LONGPRESS_MS      180
#define KEY_REPEAT_MS         80

// Matrix wiring
// Cols: GPIO10(GPIO_COL3), GPIO11(GPIO_COL2), GPIO12(GPIO_COL1)
// Rows: GPIO9(GPIO_ROW1), GPIO46(GPIO_ROW2), GPIO8(GPIO_ROW3), GPIO18(GPIO_ROW4)
static const gpio_num_t s_cols[] = { GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12 };
static const gpio_num_t s_rows[] = { GPIO_NUM_9, GPIO_NUM_46, GPIO_NUM_8, GPIO_NUM_18 };

typedef struct {
    uint8_t col_idx;
    uint8_t row_idx;
    InputKey key_default;
    bool is_white_group;
} matrix_key_def_t;

typedef struct {
    bool last_raw;
    bool stable;
    int64_t last_edge_us;
    int64_t press_start_us;
    int64_t last_repeat_us;
} key_state_t;

enum {
    kCol3 = 0, // GPIO10
    kCol2 = 1, // GPIO11
    kCol1 = 2, // GPIO12
};

enum {
    kRow1 = 0, // GPIO9
    kRow2 = 1, // GPIO46
    kRow3 = 2, // GPIO8
    kRow4 = 3, // GPIO18
};

// SW matrix mapping:
// BL group (Row3/Row4), WH group (Row2/Row1)
static const matrix_key_def_t s_key_defs[] = {
    // BL group
    { kCol2, kRow3, kInputUp,    false }, // SW8:  GPIO11 + GPIO8
    { kCol2, kRow4, kInputDown,  false }, // SW11: GPIO11 + GPIO18
    { kCol1, kRow3, kInputLeft,  false }, // SW7:  GPIO12 + GPIO8
    { kCol1, kRow4, kInputRight, false }, // SW10: GPIO12 + GPIO18
    { kCol3, kRow3, kInputEnter, false }, // SW9:  GPIO10 + GPIO8
    { kCol3, kRow4, kInputBack,  false }, // SW12: GPIO10 + GPIO18

    // WH group
    { kCol2, kRow2, kInputUp,    true },  // SW5:  GPIO11 + GPIO46
    { kCol2, kRow1, kInputDown,  true },  // SW2:  GPIO11 + GPIO9
    { kCol1, kRow1, kInputRight, true },  // SW1:  GPIO12 + GPIO9
    { kCol1, kRow2, kInputLeft,  true },  // SW4:  GPIO12 + GPIO46
    { kCol3, kRow1, kInputEnter, true },  // SW3:  GPIO10 + GPIO9
    { kCol3, kRow2, kInputBack,  true },  // SW6:  GPIO10 + GPIO46
};

#define KEY_COUNT ((int)(sizeof(s_key_defs) / sizeof(s_key_defs[0])))

static key_state_t s_key_state[KEY_COUNT];
static bool s_versus_mode = false;

static void cols_drive_all(int level)
{
    for (int i = 0; i < (int)(sizeof(s_cols) / sizeof(s_cols[0])); i++) {
        gpio_set_level(s_cols[i], level);
    }
}

static void matrix_io_init(void)
{
    gpio_config_t col_cfg = {
        .pin_bit_mask = 0,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    for (int i = 0; i < (int)(sizeof(s_cols) / sizeof(s_cols[0])); i++) {
        col_cfg.pin_bit_mask |= (1ULL << s_cols[i]);
    }
    gpio_config(&col_cfg);
    cols_drive_all(1);

    gpio_config_t row_cfg = {
        .pin_bit_mask = 0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    for (int i = 0; i < (int)(sizeof(s_rows) / sizeof(s_rows[0])); i++) {
        row_cfg.pin_bit_mask |= (1ULL << s_rows[i]);
    }
    gpio_config(&row_cfg);
}

static void scan_matrix(bool pressed[KEY_COUNT])
{
    for (int i = 0; i < KEY_COUNT; i++) {
        pressed[i] = false;
    }

    for (int c = 0; c < (int)(sizeof(s_cols) / sizeof(s_cols[0])); c++) {
        cols_drive_all(1);
        gpio_set_level(s_cols[c], 0);
        esp_rom_delay_us(3);

        for (int k = 0; k < KEY_COUNT; k++) {
            if (s_key_defs[k].col_idx != c) continue;
            int level = gpio_get_level(s_rows[s_key_defs[k].row_idx]);
            pressed[k] = (level == KEY_ACTIVE_LEVEL);
        }
    }

    cols_drive_all(1);
}

static inline int64_t ms_to_us(int64_t ms)
{
    return ms * 1000;
}

static InputKey map_versus_white_key(InputKey k)
{
    switch (k) {
        case kInputUp:    return kInputWhiteUp;
        case kInputDown:  return kInputWhiteDown;
        case kInputLeft:  return kInputWhiteLeft;
        case kInputRight: return kInputWhiteRight;
        case kInputEnter: return kInputWhiteEnter;
        case kInputBack:  return kInputWhiteBack;
        default:          return kInputNone;
    }
}

static void emit_key_for_index(int idx)
{
    const matrix_key_def_t* def = &s_key_defs[idx];
    InputKey out = kInputNone;

    if (!def->is_white_group) {
        // BL keys are always active; in versus mode they are the black side.
        out = def->key_default;
    } else if (s_versus_mode) {
        // In versus mode, WH group maps to white-side keys.
        out = map_versus_white_key(def->key_default);
    } else {
        // In normal mode, allow WH group as duplicated navigation keys.
        out = def->key_default;
    }

    if (out != kInputNone) {
        Uart1Router_InjectKey(out);
    }
}

void DrvInputGpioKeys_Init(void)
{
    matrix_io_init();

    for (int i = 0; i < KEY_COUNT; i++) {
        s_key_state[i].last_raw = false;
        s_key_state[i].stable = false;
        s_key_state[i].last_edge_us = 0;
        s_key_state[i].press_start_us = 0;
        s_key_state[i].last_repeat_us = 0;
    }
}

void DrvInputGpioKeys_Poll(void)
{
    bool raw_pressed[KEY_COUNT];
    int64_t now = esp_timer_get_time();
    scan_matrix(raw_pressed);

    for (int i = 0; i < KEY_COUNT; i++) {
        key_state_t* s = &s_key_state[i];
        bool raw = raw_pressed[i];

        if (raw != s->last_raw) {
            s->last_raw = raw;
            s->last_edge_us = now;
        }

        if ((now - s->last_edge_us) < ms_to_us(KEY_DEBOUNCE_MS)) {
            continue;
        }

        if (raw != s->stable) {
            s->stable = raw;

            if (raw) {
                s->press_start_us = now;
                s->last_repeat_us = now;
                emit_key_for_index(i);
            } else {
                s->press_start_us = 0;
                s->last_repeat_us = 0;
            }
            continue;
        }

        // Long press repeat
        if (s->stable && s->press_start_us > 0) {
            if ((now - s->press_start_us) >= ms_to_us(KEY_LONGPRESS_MS) &&
                (now - s->last_repeat_us) >= ms_to_us(KEY_REPEAT_MS)) {
                s->last_repeat_us = now;
                emit_key_for_index(i);
            }
        }
    }
}

void DrvInputGpioKeys_SetVersusMode(bool enable)
{
    s_versus_mode = enable;
}
