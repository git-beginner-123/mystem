#include "core/app_events.h"
#include "input/input.h"

bool AppEvents_Poll(AppEvent* out_event, uint32_t timeout_ms)
{
    InputKey key = kInputNone;
    if (!Input_Poll(&key, timeout_ms)) {
        return false;
    }
    out_event->key = key;
    return true;
}
