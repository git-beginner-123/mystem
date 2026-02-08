#pragma once
#include <stdbool.h>
#include "core/app_events.h"

void Input_Init(void);
bool Input_Poll(InputKey* out_key, uint32_t timeout_ms);
