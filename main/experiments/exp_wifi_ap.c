#include "experiments/experiment.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comm_wifi.h"
#include "display/st7789.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#if __has_include("esp_rom_tjpgd.h")
#include "esp_rom_tjpgd.h"
#elif __has_include("rom/tjpgd.h")
#include "rom/tjpgd.h"
#else
#error "No TJpgDec ROM header found"
#endif
#include "ui/ui.h"

static const char* kRemoteTitle = "REMOTE CAR";
static const char* TAG = "exp_remote_car";
static const char* kCarApSsid = "RC_CAR";
static const char* kCarHost = "192.168.4.1";
static const uint16_t kCarCtrlUdpPort = 3333;
static const uint32_t kReconnectEveryMs = 3000;
static const uint32_t kDisplayFrameIntervalMs = 500;
static const uint32_t kPulseRepeatMs = 60;
static const uint32_t kPulseHoldMs = 480;
static const uint32_t kAutoStopDelayMs = 620;
static const uint32_t kStreamRetryEveryMs = 1200;
static const uint32_t kHostRetryEveryMs = 3000;
static const uint8_t kHostFailThreshold = 4;

#define UI_FOOTER_H 26

typedef struct {
    const uint8_t* p;
    size_t len;
    size_t off;
    int base_x;
    int base_y;
} JpgInput;

static bool s_started = false;
static bool s_connected = false;
static bool s_host_online = false;
static bool s_has_frame = false;
static bool s_need_redraw = true;
static uint32_t s_last_connect_try_ms = 0;
static uint32_t s_auto_stop_deadline_ms = 0;
static uint32_t s_active_cmd_until_ms = 0;
static uint32_t s_next_cmd_send_ms = 0;
static uint32_t s_last_stream_try_ms = 0;
static uint32_t s_last_host_retry_ms = 0;
static uint32_t s_last_present_ms = 0;
static uint8_t s_host_fail_count = 0;
static char s_last_cmd[16] = "NONE";
static char s_active_cmd[16] = "";
static char s_status[40] = "IDLE";

static uint8_t* s_jpeg_buf = NULL;
static size_t s_jpeg_cap = 0;
static uint16_t* s_pixbuf = NULL;
static size_t s_pixbuf_cap_px = 0;
static uint8_t* s_jpeg_work = NULL;
static size_t s_jpeg_work_cap = 0;
static JpgInput s_jpg_in = {0};
static esp_http_client_handle_t s_stream_client = NULL;
static int s_ctrl_sock = -1;
static int s_last_draw_w = -1;
static int s_last_draw_h = -1;
static char s_last_footer_line[96] = "";

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static const char* input_key_name(InputKey key)
{
    switch (key) {
    case kInputUp: return "UP";
    case kInputDown: return "DOWN";
    case kInputLeft: return "LEFT";
    case kInputRight: return "RIGHT";
    case kInputEnter: return "ENTER";
    case kInputBack: return "BACK";
    default: return "UNKNOWN";
    }
}

static uint16_t c_bg(void) { return Ui_ColorRGB(8, 12, 20); }
static uint16_t c_text(void) { return Ui_ColorRGB(230, 235, 240); }
static uint16_t c_info(void) { return Ui_ColorRGB(160, 215, 255); }
static uint16_t c_warn(void) { return Ui_ColorRGB(255, 180, 110); }
static uint16_t c_ok(void) { return Ui_ColorRGB(120, 240, 120); }

static const char* jdr_name(JRESULT rc)
{
    switch (rc) {
    case JDR_OK: return "JDR_OK";
    case JDR_INTR: return "JDR_INTR";
    case JDR_INP: return "JDR_INP";
    case JDR_MEM1: return "JDR_MEM1";
    case JDR_MEM2: return "JDR_MEM2";
    case JDR_PAR: return "JDR_PAR";
    case JDR_FMT1: return "JDR_FMT1";
    case JDR_FMT2: return "JDR_FMT2";
    case JDR_FMT3: return "JDR_FMT3";
    default: return "JDR_UNKNOWN";
    }
}

static bool ensure_jpeg_buffer(size_t need)
{
    if (s_jpeg_buf && s_jpeg_cap >= need) return true;
    uint8_t* nb = (uint8_t*)realloc(s_jpeg_buf, need);
    if (!nb) return false;
    s_jpeg_buf = nb;
    s_jpeg_cap = need;
    return true;
}

static bool ensure_pixbuf_px(size_t need_px)
{
    if (s_pixbuf && s_pixbuf_cap_px >= need_px) return true;
    uint16_t* nb = (uint16_t*)realloc(s_pixbuf, need_px * sizeof(uint16_t));
    if (!nb) return false;
    s_pixbuf = nb;
    s_pixbuf_cap_px = need_px;
    return true;
}

static bool ensure_jpeg_work_fallback(void)
{
    static const size_t kTryCaps[] = {16384, 12288, 8192};
    if (s_jpeg_work && s_jpeg_work_cap >= 8192) return true;
    free(s_jpeg_work);
    s_jpeg_work = NULL;
    s_jpeg_work_cap = 0;
    for (size_t i = 0; i < sizeof(kTryCaps) / sizeof(kTryCaps[0]); i++) {
        s_jpeg_work = (uint8_t*)malloc(kTryCaps[i]);
        if (s_jpeg_work) {
            s_jpeg_work_cap = kTryCaps[i];
            return true;
        }
    }
    return false;
}

static UINT remote_jd_input(JDEC* jd, uint8_t* buff, UINT nbyte)
{
    (void)jd;
    UINT remain = (UINT)(s_jpg_in.len - s_jpg_in.off);
    if (nbyte > remain) nbyte = remain;
    if (buff && nbyte) memcpy(buff, s_jpg_in.p + s_jpg_in.off, nbyte);
    s_jpg_in.off += nbyte;
    return nbyte;
}

static UINT remote_jd_output(JDEC* jd, void* bitmap, JRECT* rect)
{
    (void)jd;
    int w = (int)(rect->right - rect->left + 1);
    int h = (int)(rect->bottom - rect->top + 1);
    if (w <= 0 || h <= 0) return 1;

    int dst_x = s_jpg_in.base_x + (int)rect->left;
    int dst_y = s_jpg_in.base_y + (int)rect->top;
    int src_x0 = 0;
    int src_y0 = 0;
    int blit_w = w;
    int blit_h = h;

    if (dst_x < 0) {
        src_x0 = -dst_x;
        blit_w -= src_x0;
        dst_x = 0;
    }
    if (dst_y < 0) {
        src_y0 = -dst_y;
        blit_h -= src_y0;
        dst_y = 0;
    }
    if (dst_x + blit_w > St7789_Width()) {
        blit_w = St7789_Width() - dst_x;
    }
    if (dst_y + blit_h > (St7789_Height() - UI_FOOTER_H)) {
        blit_h = (St7789_Height() - UI_FOOTER_H) - dst_y;
    }
    if (blit_w <= 0 || blit_h <= 0) return 1;

    size_t need_px = (size_t)blit_w * (size_t)blit_h;
    if (!ensure_pixbuf_px(need_px)) return 0;

    const uint8_t* px = (const uint8_t*)bitmap;
    uint16_t* dst = s_pixbuf;
#if defined(JD_FORMAT) && (JD_FORMAT == 1)
    for (int row = 0; row < blit_h; row++) {
        const uint8_t* src_row = px + (((src_y0 + row) * w + src_x0) * 2);
        for (int col = 0; col < blit_w; col++) {
            const uint8_t* p = src_row + col * 2;
            *dst++ = ((uint16_t)p[0] << 8) | (uint16_t)p[1];
        }
    }
#else
    for (int row = 0; row < blit_h; row++) {
        const uint8_t* src_row = px + (((src_y0 + row) * w + src_x0) * 3);
        for (int col = 0; col < blit_w; col++) {
            const uint8_t* p = src_row + col * 3;
            uint8_t r = p[0];
            uint8_t g = p[1];
            uint8_t b = p[2];
            *dst++ = (uint16_t)((r & 0xF8) << 8) | (uint16_t)((g & 0xFC) << 3) | (uint16_t)(b >> 3);
        }
    }
#endif

    St7789_BlitRect(dst_x, dst_y, blit_w, blit_h, s_pixbuf);
    return 1;
}

static void draw_footer(void)
{
    char line[96];
    int y = St7789_Height() - UI_FOOTER_H;
    snprintf(line, sizeof(line), "UP DN LT RT HOLD | %s | %s", s_last_cmd, s_status);
    if (strcmp(line, s_last_footer_line) == 0) {
        return;
    }
    snprintf(s_last_footer_line, sizeof(s_last_footer_line), "%s", line);
    St7789_FillRect(0, y, St7789_Width(), UI_FOOTER_H, c_bg());
    Ui_DrawTextAtBg(4, y + 5, line, c_text(), c_bg());
}

static void draw_waiting_screen(void)
{
    s_last_footer_line[0] = '\0';
    St7789_Fill(c_bg());
    Ui_DrawTextAtBg(8, 12, "REMOTE CAR", c_info(), c_bg());
    Ui_DrawTextAtBg(8, 34, "STA -> RC_CAR", c_info(), c_bg());
    Ui_DrawTextAtBg(8, 56, "Host: 192.168.4.1", c_info(), c_bg());
    Ui_DrawTextAtBg(8, 78, s_connected ? "WiFi: connected" : "WiFi: connecting", s_connected ? c_ok() : c_warn(), c_bg());
    Ui_DrawTextAtBg(8, 98, s_host_online ? "Host: online" : "Host: offline", s_host_online ? c_ok() : c_warn(), c_bg());
    Ui_DrawTextAtBg(8, 110, "UP=FWD DN=BWD", c_text(), c_bg());
    Ui_DrawTextAtBg(8, 132, "LT=LEFT RT=RIGHT", c_text(), c_bg());
    Ui_DrawTextAtBg(8, 154, "Release key = STOP", c_text(), c_bg());
    draw_footer();
    St7789_Flush();
}

static void close_stream_client(void)
{
    if (!s_stream_client) return;
    esp_http_client_close(s_stream_client);
    esp_http_client_cleanup(s_stream_client);
    s_stream_client = NULL;
}

static void close_ctrl_socket(void)
{
    if (s_ctrl_sock >= 0) {
        close(s_ctrl_sock);
        s_ctrl_sock = -1;
    }
}

static bool ensure_ctrl_socket(void)
{
    if (s_ctrl_sock >= 0) return true;

    s_ctrl_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_ctrl_sock < 0) {
        s_ctrl_sock = -1;
        return false;
    }
    return true;
}

static void mark_host_online(const char* status)
{
    s_host_online = true;
    s_host_fail_count = 0;
    if (status && status[0]) {
        snprintf(s_status, sizeof(s_status), "%s", status);
    }
}

static void mark_host_offline(const char* status)
{
    close_stream_client();
    s_host_fail_count++;
    if (status && status[0]) {
        snprintf(s_status, sizeof(s_status), "%s", status);
    }
    if (s_host_fail_count >= kHostFailThreshold) {
        s_host_online = false;
    }
    s_need_redraw = true;
}

static bool open_stream_client(void)
{
    char url[64];
    snprintf(url, sizeof(url), "http://%s:81/stream", kCarHost);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 2500,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .buffer_size = 1024,
        .buffer_size_tx = 256,
        .keep_alive_enable = true,
        .disable_auto_redirect = true,
    };

    close_stream_client();
    s_stream_client = esp_http_client_init(&cfg);
    if (!s_stream_client) return false;

    esp_err_t err = esp_http_client_open(s_stream_client, 0);
    if (err != ESP_OK) {
        close_stream_client();
        return false;
    }

    int hdr_len = esp_http_client_fetch_headers(s_stream_client);
    if (hdr_len < 0) {
        close_stream_client();
        return false;
    }
    mark_host_online("STREAM OPEN");
    return true;
}

static int stream_read_bytes(uint8_t* dst, size_t len)
{
    size_t total = 0;
    while (total < len) {
        int got = esp_http_client_read(s_stream_client, (char*)dst + total, (int)(len - total));
        if (got <= 0) return -1;
        total += (size_t)got;
    }
    return (int)total;
}

static bool stream_read_line(char* line, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        char ch = 0;
        int got = esp_http_client_read(s_stream_client, &ch, 1);
        if (got <= 0) return false;
        line[n++] = ch;
        if (ch == '\n') break;
    }
    line[n] = 0;
    return n > 0;
}

static bool read_stream_frame(const uint8_t** out_ptr, size_t* out_len)
{
    char line[160];
    int content_len = -1;

    *out_ptr = NULL;
    *out_len = 0;

    if (!s_stream_client && !open_stream_client()) {
        mark_host_offline("STREAM OPEN FAIL");
        return false;
    }

    for (;;) {
        if (!stream_read_line(line, sizeof(line))) {
            mark_host_offline("STREAM READ FAIL");
            return false;
        }
        if (strstr(line, "--") == line) break;
    }

    for (;;) {
        if (!stream_read_line(line, sizeof(line))) {
            mark_host_offline("STREAM HDR FAIL");
            return false;
        }
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) break;
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            content_len = atoi(line + 15);
        }
    }

    if (content_len <= 4 || content_len > 128 * 1024) {
        mark_host_offline("STREAM LEN FAIL");
        return false;
    }
    if (!ensure_jpeg_buffer((size_t)content_len)) {
        snprintf(s_status, sizeof(s_status), "JPEG BUF FAIL");
        return false;
    }
    if (stream_read_bytes(s_jpeg_buf, (size_t)content_len) < 0) {
        mark_host_offline("STREAM JPG FAIL");
        return false;
    }

    char tail[2];
    (void)stream_read_bytes((uint8_t*)tail, sizeof(tail));

    if (!(s_jpeg_buf[0] == 0xFF && s_jpeg_buf[1] == 0xD8)) {
        snprintf(s_status, sizeof(s_status), "JPEG SOI FAIL");
        return false;
    }

    *out_ptr = s_jpeg_buf;
    *out_len = (size_t)content_len;
    return true;
}

static bool send_drive_cmd(const char* cmd)
{
    ESP_LOGI(TAG, "send drive cmd: %s", cmd);

    if (!ensure_ctrl_socket()) {
        snprintf(s_status, sizeof(s_status), "UDP INIT FAIL");
        return false;
    }

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(kCarCtrlUdpPort),
        .sin_addr.s_addr = inet_addr(kCarHost),
    };
    ssize_t sent = sendto(s_ctrl_sock, cmd, strlen(cmd), 0,
                          (const struct sockaddr*)&dest, sizeof(dest));

    if (sent < 0) {
        ESP_LOGW(TAG, "drive cmd failed: cmd=%s errno=%d", cmd, errno);
        close_ctrl_socket();
        mark_host_offline("CTRL FAIL");
        return false;
    }

    snprintf(s_last_cmd, sizeof(s_last_cmd), "%s", cmd);
    ESP_LOGI(TAG, "drive cmd ok: %s", cmd);
    mark_host_online("CTRL OK");
    return true;
}

static void ensure_connected(void)
{
    uint32_t now = now_ms();
    s_connected = comm_wifi_is_connected();
    if (s_connected) return;
    s_host_online = false;
    if ((now - s_last_connect_try_ms) < kReconnectEveryMs) return;

    s_last_connect_try_ms = now;
    (void)comm_wifi_connect_psk(kCarApSsid, "");
    snprintf(s_status, sizeof(s_status), "JOIN RC_CAR");
    s_need_redraw = true;
}

static bool draw_remote_stream_frame(void)
{
    const uint8_t* jpg = NULL;
    size_t jpg_len = 0;

    if (!read_stream_frame(&jpg, &jpg_len)) {
        return false;
    }
    if (!ensure_jpeg_work_fallback()) {
        snprintf(s_status, sizeof(s_status), "JPEG MEM FAIL");
        return false;
    }

    JDEC dec;
    s_jpg_in.p = jpg;
    s_jpg_in.len = jpg_len;
    s_jpg_in.off = 0;

    JRESULT rc = jd_prepare(&dec, remote_jd_input, s_jpeg_work, (UINT)s_jpeg_work_cap, &s_jpg_in);
    if (rc != JDR_OK) {
        snprintf(s_status, sizeof(s_status), "JPEG PREP %s", jdr_name(rc));
        return false;
    }

    int body_x = 0;
    int body_y = 0;
    int body_w = St7789_Width();
    int body_h = St7789_Height() - UI_FOOTER_H;

    uint8_t scale = 0;
    int draw_w = dec.width;
    int draw_h = dec.height;
    while (scale < 3 && (draw_w > body_w * 2 || draw_h > body_h * 2)) {
        scale++;
        draw_w = (dec.width + (1 << scale) - 1) >> scale;
        draw_h = (dec.height + (1 << scale) - 1) >> scale;
    }

    s_jpg_in.base_x = body_x + (body_w - draw_w) / 2;
    s_jpg_in.base_y = body_y;
    if (!s_has_frame || draw_w != s_last_draw_w || draw_h != s_last_draw_h) {
        St7789_FillRect(body_x, body_y, body_w, body_h, c_bg());
        s_last_draw_w = draw_w;
        s_last_draw_h = draw_h;
    }

    rc = jd_decomp(&dec, remote_jd_output, scale);
    if (rc != JDR_OK) {
        snprintf(s_status, sizeof(s_status), "JPEG DRAW %s", jdr_name(rc));
        return false;
    }

    mark_host_online("STREAM OK");
    s_has_frame = true;
    draw_footer();
    St7789_Flush();
    return true;
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame(kRemoteTitle, "OK:START BACK");
    Ui_Println("Goal: STEM is STA remote.");
    Ui_Println("Join AP: RC_CAR");
    Ui_Println("Host: 192.168.4.1");
    Ui_Println("Show /stream on LCD.");
    Ui_Println("Send UP/DN/LF/RT to car.");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    comm_wifi_start();
    s_started = true;
    s_connected = false;
    s_host_online = false;
    s_has_frame = false;
    s_last_connect_try_ms = 0;
    s_auto_stop_deadline_ms = 0;
    s_active_cmd_until_ms = 0;
    s_next_cmd_send_ms = 0;
    s_last_stream_try_ms = 0;
    s_last_host_retry_ms = 0;
    s_last_present_ms = 0;
    s_host_fail_count = 0;
    s_last_draw_w = -1;
    s_last_draw_h = -1;
    s_active_cmd[0] = '\0';
    snprintf(s_last_cmd, sizeof(s_last_cmd), "NONE");
    snprintf(s_status, sizeof(s_status), "STARTING");
    s_need_redraw = true;
    ensure_connected();
    draw_waiting_screen();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    if (s_started && s_connected) {
        (void)send_drive_cmd("stop");
    }
    close_stream_client();
    close_ctrl_socket();
    s_started = false;
    s_connected = false;
    s_host_online = false;
    s_has_frame = false;
    s_last_draw_w = -1;
    s_last_draw_h = -1;
    s_auto_stop_deadline_ms = 0;
    s_active_cmd_until_ms = 0;
    s_next_cmd_send_ms = 0;
    s_active_cmd[0] = '\0';
    snprintf(s_last_cmd, sizeof(s_last_cmd), "NONE");
    snprintf(s_status, sizeof(s_status), "STOPPED");
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    const char* cmd = NULL;
    ESP_LOGI(TAG, "received key: %s", input_key_name(key));
    switch (key) {
    case kInputUp: cmd = "forward"; break;
    case kInputDown: cmd = "backward"; break;
    case kInputLeft: cmd = "left"; break;
    case kInputRight: cmd = "right"; break;
    default: break;
    }

    if (!cmd) {
        ESP_LOGI(TAG, "key ignored: %s", input_key_name(key));
        return;
    }
    ESP_LOGI(TAG, "mapped key to drive cmd: %s -> %s", input_key_name(key), cmd);
    ensure_connected();
    if (!comm_wifi_is_connected()) {
        ESP_LOGW(TAG, "wifi not ready, skip cmd=%s", cmd);
        snprintf(s_status, sizeof(s_status), "WAIT WIFI");
        s_need_redraw = true;
        return;
    }

    if (send_drive_cmd(cmd)) {
        uint32_t now = now_ms();
        snprintf(s_active_cmd, sizeof(s_active_cmd), "%s", cmd);
        s_active_cmd_until_ms = now + kPulseHoldMs;
        s_next_cmd_send_ms = now + kPulseRepeatMs;
        s_auto_stop_deadline_ms = now + kAutoStopDelayMs;
    }
    draw_footer();
    St7789_Flush();
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
    if (!s_started) return;

    bool prev_connected = s_connected;
    ensure_connected();
    s_connected = comm_wifi_is_connected();
    if (!prev_connected && s_connected) {
        close_stream_client();
        s_host_online = false;
        s_host_fail_count = 0;
        s_last_stream_try_ms = 0;
        s_last_host_retry_ms = 0;
        s_need_redraw = true;
    }

    uint32_t now = now_ms();
    if (s_active_cmd[0] && s_connected && now >= s_next_cmd_send_ms && now < s_active_cmd_until_ms) {
        (void)send_drive_cmd(s_active_cmd);
        s_next_cmd_send_ms = now + kPulseRepeatMs;
    }
    if (s_active_cmd_until_ms && now >= s_active_cmd_until_ms) {
        s_active_cmd_until_ms = 0;
        s_next_cmd_send_ms = 0;
        s_active_cmd[0] = '\0';
    }
    if (s_auto_stop_deadline_ms && now >= s_auto_stop_deadline_ms && s_connected) {
        (void)send_drive_cmd("stop");
        s_auto_stop_deadline_ms = 0;
    }

    if (!s_connected) {
        close_stream_client();
        close_ctrl_socket();
        s_host_online = false;
        s_has_frame = false;
        s_last_draw_w = -1;
        s_last_draw_h = -1;
        s_active_cmd_until_ms = 0;
        s_next_cmd_send_ms = 0;
        s_active_cmd[0] = '\0';
        s_auto_stop_deadline_ms = 0;
        if (s_need_redraw) {
            draw_waiting_screen();
            s_need_redraw = false;
        }
        return;
    }

    if (!s_host_online && (now - s_last_host_retry_ms) < kHostRetryEveryMs) {
        if (s_need_redraw) {
            if (s_has_frame) {
                draw_footer();
                St7789_Flush();
            } else {
                draw_waiting_screen();
            }
            s_need_redraw = false;
        }
        return;
    }

    if (!s_host_online) {
        s_last_host_retry_ms = now;
    }

    if ((now - s_last_stream_try_ms) >= kStreamRetryEveryMs || s_stream_client || !s_host_online) {
        s_last_stream_try_ms = now;
        if ((now - s_last_present_ms) < kDisplayFrameIntervalMs) {
            s_need_redraw = false;
            return;
        }
        if (!draw_remote_stream_frame()) {
            if (s_has_frame) {
                draw_footer();
                St7789_Flush();
            } else {
                draw_waiting_screen();
            }
        } else {
            s_last_present_ms = now;
        }
        s_need_redraw = false;
    }
}

const Experiment g_exp_wifi_ap = {
    .id = 9,
    .title = "REMOTE CAR",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
