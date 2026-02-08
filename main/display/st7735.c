#include "display/st7735.h"

#include <string.h>

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_err.h"

// Pins (adjust if needed)
#define PIN_SCK   21
#define PIN_MOSI  47
#define PIN_CS    41
#define PIN_DC    40
#define PIN_RST   45
#define PIN_BLK   42

#define LCD_HOST  SPI2_HOST
#define ST7735_W  240
#define ST7735_H  320

static const char* kTag = "ST7735";

static spi_device_handle_t s_spi;
static SemaphoreHandle_t s_lcd_mutex;

static inline void lcd_lock(void)   { xSemaphoreTake(s_lcd_mutex, portMAX_DELAY); }
static inline void lcd_unlock(void) { xSemaphoreGive(s_lcd_mutex); }

static inline void dc_cmd(void)  { gpio_set_level(PIN_DC, 0); }
static inline void dc_data(void) { gpio_set_level(PIN_DC, 1); }

// -----------------------------
// SPI helpers
// -----------------------------
static void spi_write_polling(const void* data, int len)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = len * 8;
    t.tx_buffer = data;
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void write_cmd(uint8_t cmd)
{
    dc_cmd();
    spi_write_polling(&cmd, 1);
}

static void write_data(const uint8_t* data, int len)
{
    dc_data();
    spi_write_polling(data, len);
}

static void write_u16_be(uint16_t v)
{
    uint8_t d[2] = { (uint8_t)(v >> 8), (uint8_t)(v & 0xFF) };
    write_data(d, 2);
}

static void hw_reset(void)
{
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void set_addr_window(int x0, int y0, int x1, int y1)
{
    write_cmd(0x2A);
    write_u16_be((uint16_t)x0);
    write_u16_be((uint16_t)x1);

    write_cmd(0x2B);
    write_u16_be((uint16_t)y0);
    write_u16_be((uint16_t)y1);

    write_cmd(0x2C);
}

// -----------------------------
// DMA queue for pixel payload
// -----------------------------
#define LCD_DMA_QUEUE        6
#define LCD_DMA_CHUNK_BYTES  4096  // must be <= max_transfer_sz

static uint8_t* s_dma_buf[LCD_DMA_QUEUE];
static spi_transaction_t s_dma_trans[LCD_DMA_QUEUE];
static int s_dma_buf_idx = 0;
static int s_dma_inflight = 0;

static void lcd_dma_wait_one(void)
{
    spi_transaction_t* rt = NULL;
    if (s_dma_inflight > 0) {
        ESP_ERROR_CHECK(spi_device_get_trans_result(s_spi, &rt, portMAX_DELAY));
        s_dma_inflight--;
    }
}

void St7735_Flush(void)
{
    lcd_lock();
    while (s_dma_inflight > 0) lcd_dma_wait_one();
    lcd_unlock();
}

static void lcd_dma_wait_all_locked(void)
{
    while (s_dma_inflight > 0) lcd_dma_wait_one();
}

static void lcd_dma_queue_pixels_be16(const uint16_t* pixels, int count_words)
{
    dc_data();

    const uint16_t* src = pixels;
    int remaining = count_words;

    while (remaining > 0) {
        int max_words = LCD_DMA_CHUNK_BYTES / 2;
        int nwords = remaining;
        if (nwords > max_words) nwords = max_words;

        if (s_dma_inflight >= LCD_DMA_QUEUE) {
            lcd_dma_wait_one();
        }

        uint8_t* dst = s_dma_buf[s_dma_buf_idx];
        for (int i = 0; i < nwords; i++) {
            uint16_t v = src[i];
            dst[i * 2 + 0] = (uint8_t)(v >> 8);
            dst[i * 2 + 1] = (uint8_t)(v & 0xFF);
        }

        spi_transaction_t* t = &s_dma_trans[s_dma_buf_idx];
        memset(t, 0, sizeof(*t));
        t->length = (nwords * 2) * 8;
        t->tx_buffer = dst;

        ESP_ERROR_CHECK(spi_device_queue_trans(s_spi, t, portMAX_DELAY));
        s_dma_inflight++;

        s_dma_buf_idx = (s_dma_buf_idx + 1) % LCD_DMA_QUEUE;
        src += nwords;
        remaining -= nwords;
    }
}

static void lcd_dma_queue_color565(uint16_t color565, int count_words)
{
    dc_data();

    uint8_t hi = (uint8_t)(color565 >> 8);
    uint8_t lo = (uint8_t)(color565 & 0xFF);

    int remaining = count_words;
    while (remaining > 0) {
        int max_words = LCD_DMA_CHUNK_BYTES / 2;
        int nwords = remaining;
        if (nwords > max_words) nwords = max_words;

        if (s_dma_inflight >= LCD_DMA_QUEUE) {
            lcd_dma_wait_one();
        }

        uint8_t* dst = s_dma_buf[s_dma_buf_idx];
        for (int i = 0; i < nwords; i++) {
            dst[i * 2 + 0] = hi;
            dst[i * 2 + 1] = lo;
        }

        spi_transaction_t* t = &s_dma_trans[s_dma_buf_idx];
        memset(t, 0, sizeof(*t));
        t->length = (nwords * 2) * 8;
        t->tx_buffer = dst;

        ESP_ERROR_CHECK(spi_device_queue_trans(s_spi, t, portMAX_DELAY));
        s_dma_inflight++;

        s_dma_buf_idx = (s_dma_buf_idx + 1) % LCD_DMA_QUEUE;
        remaining -= nwords;
    }
}

// -----------------------------
// Public API
// -----------------------------
int St7735_Width(void)  { return ST7735_W; }
int St7735_Height(void) { return ST7735_H; }

void St7735_Init(void)
{
    gpio_config_t io = {0};
    io.mode = GPIO_MODE_OUTPUT;
    io.pin_bit_mask =
        (1ULL << PIN_CS) |
        (1ULL << PIN_DC) |
        (1ULL << PIN_RST) |
        (1ULL << PIN_BLK);
    gpio_config(&io);

    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_BLK, 1);

    spi_bus_config_t buscfg = {0};
    buscfg.sclk_io_num = PIN_SCK;
    buscfg.mosi_io_num = PIN_MOSI;
    buscfg.miso_io_num = -1;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 32 * 1024;

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {0};
    devcfg.clock_speed_hz = 40 * 1000 * 1000; // adjust if needed
    devcfg.mode = 0;
    devcfg.spics_io_num = PIN_CS;
    devcfg.queue_size = LCD_DMA_QUEUE;

    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &devcfg, &s_spi));

    s_lcd_mutex = xSemaphoreCreateMutex();

    for (int i = 0; i < LCD_DMA_QUEUE; i++) {
        s_dma_buf[i] = (uint8_t*)heap_caps_malloc(LCD_DMA_CHUNK_BYTES, MALLOC_CAP_DMA);
        memset(&s_dma_trans[i], 0, sizeof(spi_transaction_t));
    }

    hw_reset();

    write_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(150));
    write_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(150));

    write_cmd(0x3A);
    {
        uint8_t d = 0x05; // 16-bit color
        write_data(&d, 1);
    }

    write_cmd(0x36);
    {
        uint8_t d = 0x00; // rotation, adjust if needed
        write_data(&d, 1);
    }

    write_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(kTag, "init done");
}

void St7735_DrawPixel(int x, int y, uint16_t color565)
{
    if (x < 0 || y < 0) return;
    if (x >= ST7735_W || y >= ST7735_H) return;

    lcd_lock();
    lcd_dma_wait_all_locked();

    set_addr_window(x, y, x, y);
    dc_data();

    uint8_t d[2] = { (uint8_t)(color565 >> 8), (uint8_t)(color565 & 0xFF) };
    spi_write_polling(d, 2);

    lcd_unlock();
}

void St7735_BlitRect(int x, int y, int w, int h, const uint16_t* pixels565)
{
    if (w <= 0 || h <= 0) return;
    if (x < 0 || y < 0) return;
    if (x + w > ST7735_W) return;
    if (y + h > ST7735_H) return;
    if (!pixels565) return;

    lcd_lock();
    lcd_dma_wait_all_locked();

    set_addr_window(x, y, x + w - 1, y + h - 1);
    lcd_dma_queue_pixels_be16(pixels565, w * h);

    lcd_unlock();
}

void St7735_FillRect(int x, int y, int w, int h, uint16_t color565)
{
    if (w <= 0 || h <= 0) return;
    if (x < 0 || y < 0) return;
    if (x + w > ST7735_W) return;
    if (y + h > ST7735_H) return;

    lcd_lock();
    lcd_dma_wait_all_locked();

    set_addr_window(x, y, x + w - 1, y + h - 1);
    lcd_dma_queue_color565(color565, w * h);

    lcd_unlock();
}

void St7735_Fill(uint16_t color565)
{
    lcd_lock();
    lcd_dma_wait_all_locked();

    set_addr_window(0, 0, ST7735_W - 1, ST7735_H - 1);
    lcd_dma_queue_color565(color565, ST7735_W * ST7735_H);

    lcd_unlock();
}