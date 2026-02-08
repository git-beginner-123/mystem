#include "input/input.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "input/uart1_router.h"


#define KEY_UART_NUM      UART_NUM_1
#define KEY_UART_TX_GPIO  35
#define KEY_UART_RX_GPIO  36
#define KEY_UART_BAUDRATE 115200
#define KEY_RX_BUF_SIZE   256

#define FRAME_HEAD 0xAA
#define FRAME_TAIL 0x55

typedef enum {
    kParseWaitHead = 0,
    kParseWaitCmd,
    kParseWaitTail
} ParseState;

static ParseState s_state = kParseWaitHead;
static uint8_t s_cmd = 0;

static bool cmd_to_key(uint8_t cmd, InputKey* out_key)
{
    uint8_t keycode = 0;
    switch (cmd) {
        case 0x01: keycode = 0x11; break;
        case 0x02: keycode = 0x12; break;
        case 0x03: keycode = 0x13; break;
        case 0x04: keycode = 0x14; break;
        case 0x05: keycode = 0x15; break;
        default: return false;
    }

    switch (keycode) {
        case 0x11: *out_key = kInputUp;    return true;
        case 0x12: *out_key = kInputDown;  return true;
        case 0x13: *out_key = kInputEnter; return true;
        case 0x14: *out_key = kInputBack;  return true;
        case 0x15: *out_key = kInputEnter; return true;
        default: return false;
    }
}

void Input_Init(void)
{
    Uart1Router_Init();
}

bool Input_Poll(InputKey* out_key, uint32_t timeout_ms)
{
    return Uart1Router_PollKey(out_key, timeout_ms);
}
