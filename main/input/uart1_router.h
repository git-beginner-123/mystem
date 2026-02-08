

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "core/app_events.h"

void Uart1Router_Init(void);

bool Uart1Router_PollKey(InputKey* out_key, uint32_t timeout_ms);

void Uart1Router_EnableData(bool enable);
bool Uart1Router_ReadDataByte(uint8_t* out_b, uint32_t timeout_ms);

int Uart1Router_Write(const uint8_t* data, int len);
void Uart1Router_InjectKey(InputKey key);
