#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    kCommBleOff = 0,        // disabled: no adv, no rx/tx
    kCommBleIdle,           // enabled but not advertising yet (rare)
    kCommBleAdvertising,
    kCommBleConnected,
} CommBleState;

void CommBle_InitOnce(void);

/* Enable gate: true => start advertising; false => stop adv + disconnect + clear rx */
void CommBle_Enable(bool en);
bool CommBle_IsEnabled(void);

CommBleState CommBle_GetState(void);

const char* CommBle_GetName(void);
void CommBle_GetAddrStr(char* out, int len);

int  CommBle_GetLastRx(uint8_t* out, int max_len);
int  CommBle_GetLastRxLen(void);

/* Clear last RX buffer (for UI reset) */
void CommBle_ClearLastRx(void);
