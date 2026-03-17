#include "experiments/experiment.h"
#include "ui/ui.h"
#include "display/st7789.h"

#include "esp_log.h"
#if __has_include("esp_rom_tjpgd.h")
#include "esp_rom_tjpgd.h"
#elif __has_include("rom/tjpgd.h")
#include "rom/tjpgd.h"
#else
#error "No TJpgDec ROM header found"
#endif
#include "esp_spiffs.h"
#include "esp_timer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

static const char* TAG = "EXP_SOLAR";

#define UI_HEADER_H 30
#define UI_FOOTER_H 26
#define PLAY_INTERVAL_MS_DEFAULT 120
#define PLAY_INTERVAL_MS_MIN 80
#define PLAY_INTERVAL_MS_MAX 220
#define PLAY_SPEED_FACTOR 1
#define FRAME_BUF_CAP 32768
// Keep decoder output neutral by default.
// Source clips can be pre-adjusted during transcoding if needed.
#define SOLAR_BRIGHTNESS_BOOST 0

typedef struct {
    const char* title;
    const char* path;
} SolarClip;

static const SolarClip kClips[] = {
    {"SPACE TOTAL", "/spiffs/space_total.mjpeg"},
    {"SPACE NATURAL", "/spiffs/space_total_natural_422.mjpeg"},
    {"SPACE NIGHT", "/spiffs/space_total_night_422.mjpeg"},
    {"LUNAR ECLIPSE", "/spiffs/lunar_eclipse_240.mjpeg"},
};

static int s_idx = 0;
static bool s_running = false;
static bool s_playing = true;
static bool s_spiffs_mounted = false;
static uint32_t s_next_tick_ms = 0;
static uint32_t s_target_interval_ms = PLAY_INTERVAL_MS_DEFAULT;
static bool s_title_dirty = true;

static FILE* s_fp = NULL;
static uint8_t* s_frame_buf = NULL;
static size_t s_frame_cap = 0;
static uint16_t* s_pixbuf = NULL;
static size_t s_pixbuf_cap_px = 0;
static uint8_t* s_jpeg_work = NULL;
static size_t s_jpeg_work_cap = 0;
static uint32_t s_frame_counter = 0;
static uint32_t s_fail_counter = 0;
static uint32_t s_stream_frame_counter = 0;
static uint32_t s_drop_counter = 0;
static bool s_clip_ended = false;
static char s_active_clip_path[96] = {0};
static uint8_t* s_file_io_buf = NULL;
static size_t s_file_io_cap = 0;

typedef struct {
    const uint8_t* p;
    size_t len;
    size_t off;
    int base_x;
    int base_y;
} JpgInput;

static JpgInput s_jpg_in = {0};
static bool s_logged_jpeg_info = false;

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

static bool is_supported_sampling(uint8_t nf, uint8_t hv0, uint8_t hv1, uint8_t hv2)
{
    if (nf == 1) return true;
    if (nf < 3) return false;

    uint8_t y_h = (uint8_t)(hv0 >> 4);
    uint8_t y_v = (uint8_t)(hv0 & 0x0F);
    uint8_t cb_h = (uint8_t)(hv1 >> 4);
    uint8_t cb_v = (uint8_t)(hv1 & 0x0F);
    uint8_t cr_h = (uint8_t)(hv2 >> 4);
    uint8_t cr_v = (uint8_t)(hv2 & 0x0F);

    bool uv_11 = (cb_h == 1 && cb_v == 1 && cr_h == 1 && cr_v == 1);
    bool y_420 = (y_h == 2 && y_v == 2);
    bool y_422 = (y_h == 2 && y_v == 1);
    bool y_444 = (y_h == 1 && y_v == 1);
    return uv_11 && (y_420 || y_422 || y_444);
}

static bool probe_mjpeg_first_jpeg_supported(FILE* fp, const char* path)
{
    if (!fp) return false;
    long old_pos = ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);

    int b0 = fgetc(fp);
    int b1 = fgetc(fp);
    if (b0 != 0xFF || b1 != 0xD8) {
        ESP_LOGW(TAG, "skip unsupported stream (no SOI): %s", path ? path : "(null)");
        (void)fseek(fp, old_pos, SEEK_SET);
        return false;
    }

    while (1) {
        int c = fgetc(fp);
        if (c == EOF) break;
        if (c != 0xFF) continue;

        int m = fgetc(fp);
        if (m == EOF) break;
        while (m == 0xFF) {
            m = fgetc(fp);
            if (m == EOF) break;
        }
        if (m == EOF) break;

        if (m == 0xD8 || m == 0xD9 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;

        int hi = fgetc(fp);
        int lo = fgetc(fp);
        if (hi == EOF || lo == EOF) break;
        uint16_t seglen = (uint16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
        if (seglen < 2) break;

        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            if (seglen < 8) break;
            uint8_t payload[32];
            size_t need = (size_t)(seglen - 2);
            if (need > sizeof(payload)) need = sizeof(payload);
            size_t got = fread(payload, 1, need, fp);
            if (got < 8) break;
            uint8_t nf = payload[5];
            if (nf == 1) {
                (void)fseek(fp, old_pos, SEEK_SET);
                return true;
            }
            if (nf >= 3 && got >= 15) {
                uint8_t hv0 = payload[7];
                uint8_t hv1 = payload[10];
                uint8_t hv2 = payload[13];
                bool ok = is_supported_sampling(nf, hv0, hv1, hv2);
                if (!ok) {
                    ESP_LOGW(TAG, "skip unsupported samp: %s Y=%ux%u Cb=%ux%u Cr=%ux%u",
                             path ? path : "(null)",
                             (unsigned)(hv0 >> 4), (unsigned)(hv0 & 0x0F),
                             (unsigned)(hv1 >> 4), (unsigned)(hv1 & 0x0F),
                             (unsigned)(hv2 >> 4), (unsigned)(hv2 & 0x0F));
                }
                (void)fseek(fp, old_pos, SEEK_SET);
                return ok;
            }
            break;
        }

        long skip = (long)seglen - 2L;
        if (skip > 0) {
            if (fseek(fp, skip, SEEK_CUR) != 0) break;
        }
    }

    ESP_LOGW(TAG, "skip unsupported stream (no SOF): %s", path ? path : "(null)");
    (void)fseek(fp, old_pos, SEEK_SET);
    return false;
}

static void log_jpeg_layout_once(const uint8_t* jpg, size_t len)
{
    if (s_logged_jpeg_info || !jpg || len < 32) return;
    if (!(jpg[0] == 0xFF && jpg[1] == 0xD8)) return;

    size_t p = 2;
    while (p + 4 < len) {
        if (jpg[p] != 0xFF) {
            p++;
            continue;
        }
        while (p + 1 < len && jpg[p + 1] == 0xFF) p++;
        if (p + 1 >= len) break;
        uint8_t m = jpg[p + 1];
        p += 2;

        if (m == 0xD8 || m == 0xD9 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            continue;
        }
        if (p + 1 >= len) break;
        uint16_t seglen = (uint16_t)(((uint16_t)jpg[p] << 8) | (uint16_t)jpg[p + 1]);
        if (seglen < 2 || p + seglen > len) break;

        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            if (seglen < 8) break;
            uint8_t prec = jpg[p + 2];
            uint16_t h = (uint16_t)(((uint16_t)jpg[p + 3] << 8) | (uint16_t)jpg[p + 4]);
            uint16_t w = (uint16_t)(((uint16_t)jpg[p + 5] << 8) | (uint16_t)jpg[p + 6]);
            uint8_t nf = jpg[p + 7];
            ESP_LOGI(TAG, "JPEG SOF=0x%02X p=%u %ux%u nf=%u", m, prec, (unsigned)w, (unsigned)h, nf);
            if (nf >= 3 && seglen >= 17) {
                uint8_t hv0 = jpg[p + 9];
                uint8_t hv1 = jpg[p + 12];
                uint8_t hv2 = jpg[p + 15];
                ESP_LOGI(TAG, "JPEG samp Y=%ux%u Cb=%ux%u Cr=%ux%u",
                         (unsigned)(hv0 >> 4), (unsigned)(hv0 & 0x0F),
                         (unsigned)(hv1 >> 4), (unsigned)(hv1 & 0x0F),
                         (unsigned)(hv2 >> 4), (unsigned)(hv2 & 0x0F));
            }
            s_logged_jpeg_info = true;
            return;
        }
        p += seglen;
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint16_t c_bg(void) { return Ui_ColorRGB(8, 12, 20); }
static uint16_t c_text(void) { return Ui_ColorRGB(235, 235, 235); }
static uint16_t c_warn(void) { return Ui_ColorRGB(255, 130, 130); }

static void apply_display_profile(void)
{
    St7789_ApplyPanelDefaultProfile();
}

static void close_clip_file(void)
{
    if (s_fp) {
        fclose(s_fp);
        s_fp = NULL;
    }
}

static bool ensure_frame_buffer(size_t cap)
{
    if (s_frame_buf && s_frame_cap >= cap) return true;
    if (s_frame_buf) free(s_frame_buf);
    s_frame_buf = (uint8_t*)malloc(cap);
    if (!s_frame_buf) {
        s_frame_cap = 0;
        return false;
    }
    s_frame_cap = cap;
    return true;
}

static bool ensure_frame_buffer_fallback(void)
{
    static const size_t kTryCaps[] = { 32768, 24576, 16384, 12288 };
    for (int i = 0; i < (int)(sizeof(kTryCaps) / sizeof(kTryCaps[0])); i++) {
        if (ensure_frame_buffer(kTryCaps[i])) {
            ESP_LOGI(TAG, "frame buffer=%u", (unsigned)kTryCaps[i]);
            return true;
        }
    }
    return false;
}

static bool mount_spiffs_once(void)
{
    if (s_spiffs_mounted) return true;
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount failed: %s", esp_err_to_name(err));
        return false;
    }
    s_spiffs_mounted = true;
    return true;
}

static bool ensure_file_io_buffer(size_t cap)
{
    if (s_file_io_buf && s_file_io_cap >= cap) return true;
    if (s_file_io_buf) free(s_file_io_buf);
    s_file_io_buf = (uint8_t*)malloc(cap);
    if (!s_file_io_buf) {
        s_file_io_cap = 0;
        return false;
    }
    s_file_io_cap = cap;
    return true;
}

static void configure_file_stream_buffer(FILE* fp)
{
    if (!fp || !s_file_io_buf || s_file_io_cap < 1024) return;
    (void)setvbuf(fp, (char*)s_file_io_buf, _IOFBF, s_file_io_cap);
}

static bool open_selected_clip(void)
{
    bool found_any = false;
    DIR* d = NULL;
    struct dirent* ent = NULL;

    close_clip_file();
    s_fp = fopen(kClips[s_idx].path, "rb");
    configure_file_stream_buffer(s_fp);
    if (!s_fp) {
        ESP_LOGE(TAG, "open failed: %s", kClips[s_idx].path);
        // Fallback: list /spiffs and open the first .mjpeg file.
        d = opendir("/spiffs");
        if (!d) return false;
        while ((ent = readdir(d)) != NULL) {
            const char* name = ent->d_name;
            if (!name || name[0] == '\0') continue;
            ESP_LOGI(TAG, "spiffs file: %s", name);
            found_any = true;
            size_t nlen = strlen(name);
            if (nlen >= 6 && strcmp(name + nlen - 6, ".mjpeg") == 0) {
                size_t need = strlen("/spiffs/") + strlen(name) + 1;
                if (need >= sizeof(s_active_clip_path)) {
                    ESP_LOGW(TAG, "skip long name: %s", name);
                    continue;
                }
                memcpy(s_active_clip_path, "/spiffs/", strlen("/spiffs/"));
                memcpy(s_active_clip_path + strlen("/spiffs/"), name, strlen(name) + 1);
                FILE* cand = fopen(s_active_clip_path, "rb");
                if (!cand) continue;
                configure_file_stream_buffer(cand);
                if (!probe_mjpeg_first_jpeg_supported(cand, s_active_clip_path)) {
                    fclose(cand);
                    continue;
                }
                s_fp = cand;
                ESP_LOGW(TAG, "fallback open ok: %s", s_active_clip_path);
                closedir(d);
                return true;
            }
        }
        closedir(d);
        if (!found_any) ESP_LOGW(TAG, "spiffs directory is empty");
        return false;
    }
    snprintf(s_active_clip_path, sizeof(s_active_clip_path), "%s", kClips[s_idx].path);
    return true;
}

static bool ensure_pixbuf_px(size_t px)
{
    if (s_pixbuf && s_pixbuf_cap_px >= px) return true;
    if (s_pixbuf) free(s_pixbuf);
    s_pixbuf = (uint16_t*)malloc(px * sizeof(uint16_t));
    if (!s_pixbuf) {
        s_pixbuf_cap_px = 0;
        return false;
    }
    s_pixbuf_cap_px = px;
    return true;
}

static bool ensure_jpeg_work(size_t cap)
{
    if (s_jpeg_work && s_jpeg_work_cap >= cap) return true;
    if (s_jpeg_work) free(s_jpeg_work);
    s_jpeg_work = (uint8_t*)malloc(cap);
    if (!s_jpeg_work) {
        s_jpeg_work_cap = 0;
        return false;
    }
    s_jpeg_work_cap = cap;
    return true;
}

static bool ensure_jpeg_work_fallback(void)
{
    static const size_t kTryCaps[] = { 16384, 12288, 8192 };
    for (int i = 0; i < (int)(sizeof(kTryCaps) / sizeof(kTryCaps[0])); i++) {
        if (ensure_jpeg_work(kTryCaps[i])) {
            ESP_LOGI(TAG, "jpeg work buffer=%u", (unsigned)kTryCaps[i]);
            return true;
        }
    }
    return false;
}

static UINT solar_jd_input(JDEC* jd, uint8_t* buff, UINT nbyte)
{
    JpgInput* s = (JpgInput*)jd->device;
    size_t remain = (s->off < s->len) ? (s->len - s->off) : 0;
    if ((size_t)nbyte > remain) nbyte = (UINT)remain;
    if (nbyte == 0) return 0;
    if (buff) memcpy(buff, s->p + s->off, nbyte);
    s->off += nbyte;
    return nbyte;
}

static UINT solar_jd_output(JDEC* jd, void* bitmap, JRECT* rect)
{
    JpgInput* s = (JpgInput*)jd->device;
    int w = (int)(rect->right - rect->left + 1);
    int h = (int)(rect->bottom - rect->top + 1);
    if (w <= 0 || h <= 0) return 1;
    size_t need_px = (size_t)w * (size_t)h;
    if (!ensure_pixbuf_px(need_px)) return 0;

    const uint8_t* px = (const uint8_t*)bitmap;
    uint16_t* dst = s_pixbuf;
#if defined(JD_FORMAT) && (JD_FORMAT == 1)
    size_t npx = (size_t)w * (size_t)h;
    for (size_t i = 0; i < npx; i++) {
        *dst++ = ((uint16_t)px[0] << 8) | (uint16_t)px[1];
        px += 2;
    }
#else
    size_t npx = (size_t)w * (size_t)h;
    for (size_t i = 0; i < npx; i++) {
            uint8_t r = px[0];
            uint8_t g = px[1];
            uint8_t b = px[2];
#if SOLAR_BRIGHTNESS_BOOST
            // Lift dark scenes for this small LCD (space clips are mostly near-black).
            int rr = (int)r * 2 + 12;
            int gg = (int)g * 2 + 12;
            int bb = (int)b * 2 + 12;
            if (rr > 255) rr = 255;
            if (gg > 255) gg = 255;
            if (bb > 255) bb = 255;
            r = (uint8_t)rr;
            g = (uint8_t)gg;
            b = (uint8_t)bb;
#endif
            *dst++ = (uint16_t)((r & 0xF8) << 8) | (uint16_t)((g & 0xFC) << 3) | (uint16_t)(b >> 3);
            px += 3;
    }
#endif
    int dx = s->base_x + (int)rect->left;
    int dy = s->base_y + (int)rect->top;
    St7789_BlitRect(dx, dy, w, h, s_pixbuf);
    return 1;
}

static void draw_status_line(const char* line, uint16_t col)
{
    St7789_FillRect(0, UI_HEADER_H, St7789_Width(), 22, c_bg());
    Ui_DrawTextAtBg(6, UI_HEADER_H + 4, line, col, c_bg());
    St7789_Flush();
}

static void draw_debug_counter(void)
{
    char d[28];
    // D: decoded/displayed frame count, R: raw frame read count
    Ui_DrawTextAtBg(168, UI_HEADER_H + 4, "            ", c_warn(), c_bg());
    snprintf(d, sizeof(d), "D%lu R%lu", (unsigned long)s_frame_counter, (unsigned long)s_stream_frame_counter);
    Ui_DrawTextAtBg(168, UI_HEADER_H + 4, d, c_warn(), c_bg());
}

static bool read_next_jpeg_frame(const uint8_t** out_ptr, size_t* out_len)
{
    if (!s_fp || !s_frame_buf || s_frame_cap < 8) return false;

    int prev = -1;
    int ch = 0;
    int retry = 0;

    // Find SOI
    while (1) {
        ch = fgetc(s_fp);
        if (ch == EOF) {
            if (retry++ > 0) {
                s_clip_ended = true;
                return false;
            }
            s_clip_ended = true;
            return false;
        }
        if (prev == 0xFF && ch == 0xD8) break;
        prev = ch;
    }

    size_t n = 0;
    s_frame_buf[n++] = 0xFF;
    s_frame_buf[n++] = 0xD8;
    prev = 0xD8;
    bool overflow = false;

    while (1) {
        ch = fgetc(s_fp);
        if (ch == EOF) {
            // End of stream: keep last frame on screen and stop playback.
            s_clip_ended = true;
            return false;
        }
        if (!overflow) {
            if (n < s_frame_cap) {
                s_frame_buf[n++] = (uint8_t)ch;
            } else {
                overflow = true;
            }
        }
        if (prev == 0xFF && ch == 0xD9) {
            if (overflow) {
                ESP_LOGW(TAG, "frame > %u bytes, skipped", (unsigned)s_frame_cap);
                // Keep stream position advancing, do not rewind to first frame.
                return false;
            }
            s_stream_frame_counter++;
            *out_ptr = s_frame_buf;
            *out_len = n;
            return true;
        }
        prev = ch;
    }
}

static bool draw_current_frame(void)
{
    uint32_t t0 = now_ms();
    const uint8_t* jpg = NULL;
    size_t jpg_len = 0;
    if (!read_next_jpeg_frame(&jpg, &jpg_len)) return false;
    log_jpeg_layout_once(jpg, jpg_len);
    s_jpg_in.p = jpg;
    s_jpg_in.len = jpg_len;
    s_jpg_in.off = 0;
    s_jpg_in.base_x = 0;
    s_jpg_in.base_y = UI_HEADER_H + 24;

    // ESP ROM TJpgDec needs a relatively large workspace for some valid baseline JPEGs.
    // 4KB can fail on certain Huffman/table layouts even when frame size is small.
    if (!s_jpeg_work || s_jpeg_work_cap < 8192) return false;
    JDEC dec;
    JRESULT rc;

    rc = jd_prepare(&dec, solar_jd_input, s_jpeg_work, (UINT)s_jpeg_work_cap, &s_jpg_in);
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "jd_prepare err=%d(%s) (jpg_len=%u)",
                 (int)rc, jdr_name(rc), (unsigned)jpg_len);
        return false;
    }
    rc = jd_decomp(&dec, solar_jd_output, 0);
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "jd_decomp err=%d", (int)rc);
        return false;
    }
    s_frame_counter++;
    uint32_t dt = now_ms() - t0;
    // Adapt playback cadence to real decode time to reduce catch-up frame drops.
    // Use light smoothing and keep a small headroom margin.
    uint32_t next_target = dt + 10U;
    if (next_target < PLAY_INTERVAL_MS_MIN) next_target = PLAY_INTERVAL_MS_MIN;
    if (next_target > PLAY_INTERVAL_MS_MAX) next_target = PLAY_INTERVAL_MS_MAX;
    s_target_interval_ms = (uint32_t)((s_target_interval_ms * 3U + next_target) / 4U);
    if ((s_frame_counter % 30U) == 0U) {
        ESP_LOGI(TAG, "decode+draw=%lums frame=%lu drop=%lu target=%lums",
                 (unsigned long)dt,
                 (unsigned long)s_frame_counter,
                 (unsigned long)s_drop_counter,
                 (unsigned long)s_target_interval_ms);
    }
    St7789_Flush();
    return true;
}

static void draw_clip_title(void)
{
    if (!s_title_dirty) return;
    char line[64];
    snprintf(line, sizeof(line), "%d/%d %s",
             s_idx + 1, (int)(sizeof(kClips) / sizeof(kClips[0])), kClips[s_idx].title);
    draw_status_line(line, c_text());
    draw_debug_counter();
    St7789_Flush();
    s_title_dirty = false;
}

static void select_clip(int idx)
{
    int n = (int)(sizeof(kClips) / sizeof(kClips[0]));
    if (n <= 0) return;
    while (idx < 0) idx += n;
    while (idx >= n) idx -= n;
    s_idx = idx;
    if (!open_selected_clip()) {
        draw_status_line("Clip open failed", c_warn());
        s_running = false;
        return;
    }
    s_clip_ended = false;
    s_title_dirty = true;
    draw_clip_title();
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("SOLAR MJPEG", "UP/DN:CLIP OK:PLAY/PAUSE");
    Ui_Println("Direct MJPEG playback.");
    Ui_Println("Files from /spiffs/*.mjpeg");
    Ui_Println("UP/DN switch clip.");
    Ui_Println("OK pause/resume.");
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_enter");
    apply_display_profile();
    s_idx = 0;
    s_playing = true;
    s_title_dirty = true;
}

static void exp_on_exit(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_exit");
    apply_display_profile();
    s_running = false;
    close_clip_file();
    if (s_frame_buf) { free(s_frame_buf); s_frame_buf = NULL; s_frame_cap = 0; }
    if (s_pixbuf) { free(s_pixbuf); s_pixbuf = NULL; s_pixbuf_cap_px = 0; }
    if (s_jpeg_work) { free(s_jpeg_work); s_jpeg_work = NULL; s_jpeg_work_cap = 0; }
    if (s_file_io_buf) { free(s_file_io_buf); s_file_io_buf = NULL; s_file_io_cap = 0; }
    if (s_spiffs_mounted) {
        esp_vfs_spiffs_unregister("spiffs");
        s_spiffs_mounted = false;
    }
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "start");
    apply_display_profile();
    ESP_LOGI(TAG, "play interval_default=%dms speed_factor=%d",
             PLAY_INTERVAL_MS_DEFAULT, PLAY_SPEED_FACTOR);
    ESP_LOGI(TAG, "brightness_boost=%d", SOLAR_BRIGHTNESS_BOOST);
    Ui_DrawFrame("SOLAR MJPEG", "UP/DN:CLIP OK:PLAY/PAUSE");

    if (!mount_spiffs_once()) {
        draw_status_line("SPIFFS init failed", c_warn());
        s_running = false;
        return;
    }
    if (!ensure_file_io_buffer(8192)) {
        draw_status_line("File io buffer alloc fail", c_warn());
        s_running = false;
        return;
    }
    ESP_LOGI(TAG, "file io buffer=%u", (unsigned)s_file_io_cap);
    if (!ensure_frame_buffer_fallback()) {
        draw_status_line("Frame buffer alloc fail", c_warn());
        s_running = false;
        return;
    }
    if (!ensure_jpeg_work_fallback()) {
        draw_status_line("Jpeg work alloc fail", c_warn());
        s_running = false;
        return;
    }
    if (!open_selected_clip()) {
        draw_status_line("Clip open failed", c_warn());
        s_running = false;
        return;
    }

    s_running = true;
    s_target_interval_ms = PLAY_INTERVAL_MS_DEFAULT;
    s_next_tick_ms = 0;
    s_frame_counter = 0;
    s_fail_counter = 0;
    s_stream_frame_counter = 0;
    s_drop_counter = 0;
    s_clip_ended = false;
    s_logged_jpeg_info = false;
    s_title_dirty = true;
    draw_clip_title();
    (void)draw_current_frame();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "stop");
    s_running = false;
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (!s_running) return;

    if (key == kInputUp) {
        select_clip(s_idx - 1);
        if (s_running) (void)draw_current_frame();
    } else if (key == kInputDown) {
        select_clip(s_idx + 1);
        if (s_running) (void)draw_current_frame();
    } else if (key == kInputEnter) {
        s_playing = !s_playing;
        if (!s_playing) draw_status_line("PAUSED (OK resume)", c_warn());
        else {
            s_title_dirty = true;
            draw_clip_title();
        }
    }
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
    if (!s_running || !s_playing) return;
    uint32_t t = now_ms();
    uint32_t interval = s_target_interval_ms;
    if (interval < PLAY_INTERVAL_MS_MIN) interval = PLAY_INTERVAL_MS_MIN;
    if (interval > PLAY_INTERVAL_MS_MAX) interval = PLAY_INTERVAL_MS_MAX;
    if (s_next_tick_ms == 0) s_next_tick_ms = t;
    if (t < s_next_tick_ms) return;
    uint32_t behind = t - s_next_tick_ms;
    // If we are far behind (for example after a long decode spike), reset phase
    // instead of forcing multiple catch-up skips in a row.
    if (behind > (interval * 3U)) {
        s_next_tick_ms = t;
        behind = 0;
    }
    uint32_t catchup_slots = behind / interval;
    if (catchup_slots > 3U) catchup_slots = 3U; // Avoid long catch-up loops on slow clips.

    // Keep target cadence. Skip input frames when decode/display falls behind.
    uint32_t skip_frames = (uint32_t)(PLAY_SPEED_FACTOR > 1 ? (PLAY_SPEED_FACTOR - 1) : 0);
    skip_frames += catchup_slots;
    for (uint32_t i = 0; i < skip_frames; i++) {
        const uint8_t* dummy_ptr = NULL;
        size_t dummy_len = 0;
        (void)read_next_jpeg_frame(&dummy_ptr, &dummy_len);
    }
    s_drop_counter += skip_frames;
    s_next_tick_ms += (catchup_slots + 1U) * interval;

    if (!draw_current_frame()) {
        s_fail_counter++;
        if (s_clip_ended) {
            s_playing = false;
            draw_status_line("END", c_warn());
        }
    }
}

const Experiment g_exp_solar = {
    .id = 17,
    .title = "SOLAR",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
