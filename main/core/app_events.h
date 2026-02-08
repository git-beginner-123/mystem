#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    kInputNone = 0,
    kInputUp,
    kInputDown,
    kInputEnter,
    kInputBack
} InputKey;

typedef struct {
    InputKey key;
} AppEvent;

bool AppEvents_Poll(AppEvent* out_event, uint32_t timeout_ms);
