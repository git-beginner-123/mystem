#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    kRemoteCmdNone = 0,
    kRemoteCmdUp,
    kRemoteCmdDown,
    kRemoteCmdLeft,
    kRemoteCmdRight,
    kRemoteCmdStart,
    kRemoteCmdStop,
} RemoteCmd;

bool RemoteWeb_Start(void);
void RemoteWeb_Stop(void);
RemoteCmd RemoteWeb_PopCmd(void);
bool RemoteWeb_DisplayOn(void);

#ifdef __cplusplus
}
#endif
