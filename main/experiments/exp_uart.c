#include "experiments/experiment.h"
#include "ui/ui.h"

#include "input/uart1_router.h"
#include "core/app_events.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdbool.h>

#define PKT_HEAD 0xBB
#define PKT_TAIL 0x66

typedef enum {
    kWaitHead = 0,
    kWaitLen,
    kWaitData,
    kWaitSum,
    kWaitTail
} ParseState;

typedef struct {
    bool running;
    TaskHandle_t task;

    ParseState st;
    uint8_t len;
    uint8_t pos;
    uint8_t data[256];
    uint8_t sum;

    uint32_t ok_count;
    uint32_t err_count;
    uint32_t drop_count;

    uint32_t last_ui_ms;
} UartExp;

static UartExp s_exp = {0};

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * (1000 / configTICK_RATE_HZ));
}

static uint8_t calc_sum(uint8_t len, const uint8_t* data)
{
    uint32_t s = 0;
    for (uint32_t i = 0; i < len; i++) s += data[i];
    return (uint8_t)(s & 0xFF);
}

static void send_reply_invert(const uint8_t* data, uint8_t len)
{
    uint8_t out_data[256];
    for (uint32_t i = 0; i < len; i++) out_data[i] = (uint8_t)(~data[i]);

    uint8_t out_sum = calc_sum(len, out_data);

    uint8_t b0 = PKT_HEAD;
    uint8_t b1 = len;
    uint8_t b2 = out_sum;
    uint8_t b3 = PKT_TAIL;

    Uart1Router_Write(&b0, 1);
    Uart1Router_Write(&b1, 1);
    if (len > 0) Uart1Router_Write(out_data, len);
    Uart1Router_Write(&b2, 1);
    Uart1Router_Write(&b3, 1);
}
static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (key == kInputEnter) {
        s_exp.ok_count = 0;
        s_exp.err_count = 0;
        s_exp.drop_count = 0;
        // If you have the new UI style, call your redraw here:
        // ui_draw_static(); or ui_update_status_only();
        // Otherwise:
        // ui_draw();
    }
}

static void parser_reset(void)
{
    s_exp.st = kWaitHead;
    s_exp.len = 0;
    s_exp.pos = 0;
    s_exp.sum = 0;
}

static void ui_draw_static(void)
{
    Ui_Clear();
    Ui_Println("UART EXP (UART1)");
    Ui_Println("PKT: BB LEN DATA SUM 66");
    Ui_Println("");

    Ui_Println("HEAD    :");
    Ui_Println("LEN     :");
    Ui_Println("DATA    :");
    Ui_Println("CHECKSUM:");
    Ui_Println("TAIL    :");
    Ui_Println("STATUS  :");

    Ui_Println("");
    Ui_Println("ENTER=CLR BACK=EXIT");
}

static void ui_update_last_packet(uint8_t head, uint8_t len,
                                  const uint8_t* data, uint8_t sum,
                                  uint8_t tail, bool ok)
{
    // Redraw the 6 lines only (simple approach: clear + reprint static + packet)
    // If you want true partial line redraw later, we can add Ui_DrawTextAt().
    Ui_Clear();
    Ui_Println("UART EXP (UART1)");
    Ui_Println("PKT: BB LEN DATA SUM 66");
    Ui_Println("");

    char line[128];

    snprintf(line, sizeof(line), "HEAD    : 0x%02X", (unsigned)head);
    Ui_Println(line);

    snprintf(line, sizeof(line), "LEN     : %u", (unsigned)len);
    Ui_Println(line);

    // Show up to first 16 bytes to fit the screen
    int shown = (len > 16) ? 16 : (int)len;
    int pos = 0;
    pos += snprintf(line + pos, sizeof(line) - pos, "DATA    : ");
    for (int i = 0; i < shown; i++) {
        pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", (unsigned)data[i]);
        if (pos > (int)sizeof(line) - 4) break;
    }
    if (len > 16) {
        pos += snprintf(line + pos, sizeof(line) - pos, "...");
    }
    Ui_Println(line);

    snprintf(line, sizeof(line), "CHECKSUM: 0x%02X", (unsigned)sum);
    Ui_Println(line);

    snprintf(line, sizeof(line), "TAIL    : 0x%02X", (unsigned)tail);
    Ui_Println(line);

    snprintf(line, sizeof(line), "STATUS  : %s", ok ? "OK" : "ERR");
    Ui_Println(line);

    Ui_Println("");
    Ui_Println("ENTER=CLR BACK=EXIT");
}


static void exp_task(void* arg)
{
    (void)arg;

    parser_reset();
    s_exp.last_ui_ms = 0;
    ui_draw_static();

    while (s_exp.running) {
        // Keys always available via router
        
        uint8_t b = 0;
        bool got = Uart1Router_ReadDataByte(&b, 20);
        if (!got) {
            if (s_exp.st != kWaitHead) {
                s_exp.drop_count++;
                parser_reset();
                ui_draw_static();
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        switch (s_exp.st) {
            case kWaitHead:
                if (b == PKT_HEAD) s_exp.st = kWaitLen;
                break;

            case kWaitLen:
                s_exp.len = b;
                s_exp.pos = 0;
                s_exp.st = (s_exp.len == 0) ? kWaitSum : kWaitData;
                break;

            case kWaitData:
                s_exp.data[s_exp.pos++] = b;
                if (s_exp.pos >= s_exp.len) s_exp.st = kWaitSum;
                break;

            case kWaitSum:
                s_exp.sum = b;
                s_exp.st = kWaitTail;
                break;

            case kWaitTail:
                bool ok = false;
uint8_t head = PKT_HEAD;
uint8_t tail = b; // received tail byte

if (b == PKT_TAIL) {
    uint8_t expect = calc_sum(s_exp.len, s_exp.data);
    ok = (expect == s_exp.sum);
    if (ok) {
        s_exp.ok_count++;
        send_reply_invert(s_exp.data, s_exp.len);
    } else {
        s_exp.err_count++;
    }
} else {
    s_exp.err_count++;
}

ui_update_last_packet(head, s_exp.len, s_exp.data, s_exp.sum, tail, ok);
parser_reset();

                break;

            default:
                parser_reset();
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    vTaskDelete(NULL);
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_Clear();
    Ui_Println("UART Requirements");
    Ui_Println("UART1 GPIO35/36");
    Ui_Println("RX: BB LEN DATA SUM 66");
    Ui_Println("SUM=sum(DATA)&0xFF");
    Ui_Println("TX: invert(DATA) reply");
    Ui_Println("");
    Ui_Println("ENTER=CLR BACK=RET");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;

    if (s_exp.running) return;
    s_exp.running = true;

    Uart1Router_EnableData(true);
    xTaskCreate(exp_task, "uart_exp", 4096, NULL, 10, &s_exp.task);
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;

    if (!s_exp.running) return;
    s_exp.running = false;
    vTaskDelay(pdMS_TO_TICKS(50));
    Uart1Router_EnableData(false);
}

const Experiment g_exp_uart = {
    .id = 4,
    .title = "UART",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = 0,
};
