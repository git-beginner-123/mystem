#include "input/uart1_router.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
static const char* TAG = "U1R";
static uint32_t s_rx_bytes = 0;
static uint32_t s_key_hits = 0;



#define ROUTER_UART_NUM     UART_NUM_1
#define ROUTER_TX_GPIO      35
#define ROUTER_RX_GPIO      36
#define ROUTER_BAUDRATE     115200

#define KEY_HEAD            0xAA
#define KEY_TAIL            0x55

typedef enum {
    kWaitHead = 0,
    kWaitCmd,
    kWaitTail
} KeyParseState;

static QueueHandle_t s_key_q;
static QueueHandle_t s_data_q;
static volatile bool s_data_enabled = false;

static KeyParseState s_state = kWaitHead;
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

static void data_push(uint8_t b)
{
    if (!s_data_enabled) return;
    (void)xQueueSend(s_data_q, &b, 0);
}

static void rx_task(void* arg)
{
    (void)arg;

    uint8_t b = 0;
    while (1) {
        int n = uart_read_bytes(ROUTER_UART_NUM, &b, 1, pdMS_TO_TICKS(50));
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        switch (s_state) {
            case kWaitHead:
                if (b == KEY_HEAD) {
                    s_state = kWaitCmd;
                } else {
                    data_push(b);
                }
                break;

            case kWaitCmd:
                s_cmd = b;
                s_state = kWaitTail;
                break;

            case kWaitTail:
                if (b == KEY_TAIL) {
                    InputKey k;
                    if (cmd_to_key(s_cmd, &k)) {
                        (void)xQueueSend(s_key_q, &k, 0);
                        s_key_hits++;
                        ESP_LOGI(TAG, "key frame: AA %02X 55  hits=%lu", (unsigned)s_cmd, (unsigned long)s_key_hits);

                    }
                    s_rx_bytes++;
                    if ((s_rx_bytes % 64) == 0) {
                        ESP_LOGI(TAG, "rx_bytes=%lu", (unsigned long)s_rx_bytes);
                    }

                } else {
                    // Not a valid key frame, forward bytes as data:
                    data_push(KEY_HEAD);
                    data_push(s_cmd);
                    data_push(b);
                }
                s_state = kWaitHead;
                break;

            default:
                s_state = kWaitHead;
                break;
        }
    }
}

void Uart1Router_Init(void)
{
    s_key_q  = xQueueCreate(16, sizeof(InputKey));
    s_data_q = xQueueCreate(1024, sizeof(uint8_t));

    uart_config_t cfg = {
        .baud_rate = ROUTER_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_driver_install(ROUTER_UART_NUM, 4096, 0, 0, NULL, 0);
    uart_param_config(ROUTER_UART_NUM, &cfg);
    uart_set_pin(ROUTER_UART_NUM, ROUTER_TX_GPIO, ROUTER_RX_GPIO,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    s_state = kWaitHead;
    s_cmd = 0;
    s_data_enabled = false;


    ESP_LOGI("UARTDBG", "install=%s param=%s setpin=%s",
         esp_err_to_name(uart_driver_install(UART_NUM_1, 4096, 0, 0, NULL, 0)),
         esp_err_to_name(uart_param_config(UART_NUM_1, &cfg)),
         esp_err_to_name(uart_set_pin(UART_NUM_1, 35, 36, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE)));

    xTaskCreate(rx_task, "uart1_rx_router", 4096, NULL, 12, NULL);
}

bool Uart1Router_PollKey(InputKey* out_key, uint32_t timeout_ms)
{
    return xQueueReceive(s_key_q, out_key, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void Uart1Router_EnableData(bool enable)
{
    s_data_enabled = enable;

    if (!enable) {
        uint8_t b;
        while (xQueueReceive(s_data_q, &b, 0) == pdTRUE) {}
    }
}

bool Uart1Router_ReadDataByte(uint8_t* out_b, uint32_t timeout_ms)
{
    return xQueueReceive(s_data_q, out_b, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

int Uart1Router_Write(const uint8_t* data, int len)
{
    return uart_write_bytes(ROUTER_UART_NUM, (const char*)data, len);
}

void Uart1Router_InjectKey(InputKey key)
{
    if (!s_key_q) return;
    (void)xQueueSend(s_key_q, &key, 0);
}