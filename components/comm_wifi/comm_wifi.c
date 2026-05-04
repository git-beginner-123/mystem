#include "comm_wifi.h"

#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "comm_wifi";

static EventGroupHandle_t s_wifi_evt;
static const int WIFI_CONNECTED_BIT = BIT0;
static const bool kEnableUdpEcho = false;

static TaskHandle_t s_udp_task = NULL;
static esp_netif_t *s_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static bool s_has_target = false;
static char s_target_ssid[33];
static bool s_started = false;
static bool s_scan_in_progress = false;
static TickType_t s_last_reconnect_tick = 0;
#define MAX_SCAN_REC 24
static wifi_ap_record_t s_scan_aps[MAX_SCAN_REC];
static int s_last_disc_reason = 0;
#define WIFI_DISC_NO_AP_FOUND 201

#define COMM_WIFI_NVS_NS "comm_wifi"
#define COMM_WIFI_NVS_SLOT_KEY_FMT "slot%d"

typedef struct {
    uint8_t valid;
    char ssid[33];
    char password[65];
} SavedWifiCred;

static void ensure_dns_from_got_ip(const ip_event_got_ip_t* e)
{
    if (!s_netif || !e) return;

    esp_netif_dns_info_t dns = {0};
    if (esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        if (dns.ip.type != IPADDR_TYPE_V4 || dns.ip.u_addr.ip4.addr == 0) {
            dns.ip.type = IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4 = e->ip_info.gw;
            (void)esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
        }
    }

    esp_netif_dns_info_t dns_bak = {0};
    dns_bak.ip.type = IPADDR_TYPE_V4;
    dns_bak.ip.u_addr.ip4.addr = inet_addr("8.8.8.8");
    (void)esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_BACKUP, &dns_bak);
}

static void sort_rssi_desc_small(wifi_ap_record_t* a, int n)
{
    for (int i = 0; i < n - 1; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++) {
            if (a[j].rssi > a[best].rssi) best = j;
        }
        if (best != i) {
            wifi_ap_record_t t = a[i];
            a[i] = a[best];
            a[best] = t;
        }
    }
}

static void request_reconnect(bool force_now)
{
    if (s_scan_in_progress) return;
    if (!s_has_target || !s_target_ssid[0]) return;
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK) return;
    if (!(mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA)) return;

    TickType_t now = xTaskGetTickCount();
    const TickType_t min_gap = pdMS_TO_TICKS(5000);
    if (!force_now && (now - s_last_reconnect_tick) < min_gap) return;

    s_last_reconnect_tick = now;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "reconnect failed: %s", esp_err_to_name(err));
    }
}

static bool nvs_load_saved_creds(SavedWifiCred out[COMM_WIFI_MAX_CREDENTIALS], int* out_count)
{
    if (!out || !out_count) return false;
    memset(out, 0, sizeof(SavedWifiCred) * COMM_WIFI_MAX_CREDENTIALS);
    *out_count = 0;

    nvs_handle_t h;
    esp_err_t err = nvs_open(COMM_WIFI_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return false;

    for (int i = 0; i < COMM_WIFI_MAX_CREDENTIALS; i++) {
        char key[12];
        snprintf(key, sizeof(key), COMM_WIFI_NVS_SLOT_KEY_FMT, i);
        size_t sz = sizeof(SavedWifiCred);
        SavedWifiCred c = {0};
        if (nvs_get_blob(h, key, &c, &sz) == ESP_OK && sz == sizeof(SavedWifiCred) && c.valid && c.ssid[0]) {
            c.ssid[sizeof(c.ssid) - 1] = 0;
            c.password[sizeof(c.password) - 1] = 0;
            out[*out_count] = c;
            (*out_count)++;
        }
    }
    nvs_close(h);
    return true;
}

static bool nvs_save_saved_creds(const SavedWifiCred in[COMM_WIFI_MAX_CREDENTIALS], int count)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(COMM_WIFI_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return false;

    for (int i = 0; i < COMM_WIFI_MAX_CREDENTIALS; i++) {
        char key[12];
        snprintf(key, sizeof(key), COMM_WIFI_NVS_SLOT_KEY_FMT, i);
        if (i < count) {
            if (nvs_set_blob(h, key, &in[i], sizeof(SavedWifiCred)) != ESP_OK) {
                nvs_close(h);
                return false;
            }
        } else {
            (void)nvs_erase_key(h, key);
        }
    }

    err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static bool set_sta_config_and_connect(const char* ssid, const char* password)
{
    if (!ssid || !ssid[0] || !password) return false;

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) return false;

    strncpy(s_target_ssid, ssid, sizeof(s_target_ssid) - 1);
    s_target_ssid[sizeof(s_target_ssid) - 1] = 0;
    s_has_target = true;

    return (esp_wifi_connect() == ESP_OK);
}

static bool ssid_in_scan_list(const char* ssid)
{
    uint16_t ap_num = 0;
    if (esp_wifi_scan_get_ap_num(&ap_num) != ESP_OK || ap_num == 0) return false;
    uint16_t fetch_num = ap_num;
    if (fetch_num > MAX_SCAN_REC) fetch_num = MAX_SCAN_REC;
    if (fetch_num == 0) return false;

    if (esp_wifi_scan_get_ap_records(&fetch_num, s_scan_aps) != ESP_OK) return false;

    for (uint16_t i = 0; i < fetch_num; i++) {
        if (strcmp((const char*)s_scan_aps[i].ssid, ssid) == 0) return true;
    }
    return false;
}

static int wifi_scan_list(CommWifiAp* out, int cap)
{
    if (!out || cap <= 0) return 0;
    s_scan_in_progress = true;
    (void)esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(120));

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true /* block */);
    if (err == ESP_ERR_WIFI_STATE) {
        vTaskDelay(pdMS_TO_TICKS(220));
        err = esp_wifi_scan_start(&scan_cfg, true /* block */);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
        s_scan_in_progress = false;
        request_reconnect(true);
        return 0;
    }

    uint16_t ap_num = 0;
    if (esp_wifi_scan_get_ap_num(&ap_num) != ESP_OK || ap_num == 0) {
        ESP_LOGI(TAG, "scan: no APs found");
        s_scan_in_progress = false;
        request_reconnect(true);
        return 0;
    }

    wifi_ap_record_t* aps = s_scan_aps;
    uint16_t fetch_num = ap_num;
    if (fetch_num > MAX_SCAN_REC) fetch_num = MAX_SCAN_REC;
    if (fetch_num == 0) {
        s_scan_in_progress = false;
        request_reconnect(true);
        return 0;
    }

    int count = 0;
    if (esp_wifi_scan_get_ap_records(&fetch_num, aps) == ESP_OK) {
        ESP_LOGI(TAG, "scan: total=%u, used=%u", (unsigned)ap_num, (unsigned)fetch_num);
        for (uint16_t i = 0; i < fetch_num; i++) {
            const char *auth = (aps[i].authmode == WIFI_AUTH_OPEN) ? "OPEN" : "SEC";
            ESP_LOGI(TAG, "  %02u) %s  RSSI=%d  %s", i + 1, (char *)aps[i].ssid, aps[i].rssi, auth);
        }

        int count_all = 0;
        for (uint16_t i = 0; i < fetch_num; i++) {
            if (aps[i].ssid[0]) {
                aps[count_all++] = aps[i];
            }
        }

        if (count_all > 0) {
            sort_rssi_desc_small(aps, count_all);
            int pick = (count_all > cap) ? cap : count_all;
            for (int i = 0; i < pick; i++) {
                strncpy(out[i].ssid, (char *)aps[i].ssid, sizeof(out[i].ssid) - 1);
                out[i].ssid[sizeof(out[i].ssid) - 1] = 0;
                out[i].rssi = aps[i].rssi;
            }
            count = pick;
        }
    }

    s_scan_in_progress = false;
    request_reconnect(true);
    return count;
}

static void udp_echo_task(void *arg)
{
    const int port = CONFIG_COMM_WIFI_UDP_PORT;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGE(TAG, "bind() failed: errno=%d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UDP echo server listening on port %d", port);

    while (1) {
        char rxbuf[256];
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);

        int len = recvfrom(sock, rxbuf, sizeof(rxbuf), 0,
                           (struct sockaddr *)&source_addr, &socklen);
        if (len < 0) {
            ESP_LOGE(TAG, "recvfrom() failed: errno=%d", errno);
            break;
        }

        char addr_str[64];
        inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str));
        ESP_LOGI(TAG, "RX %d bytes from %s:%d", len, addr_str, ntohs(source_addr.sin_port));

        int sent = sendto(sock, rxbuf, len, 0,
                          (struct sockaddr *)&source_addr, sizeof(source_addr));
        if (sent < 0) {
            ESP_LOGE(TAG, "sendto() failed: errno=%d", errno);
            break;
        }
        ESP_LOGI(TAG, "TX %d bytes", sent);
    }

    close(sock);
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_START");
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* e = (wifi_event_sta_disconnected_t*)data;
        s_last_disc_reason = e ? (int)e->reason : 0;
        wifi_mode_t mode = WIFI_MODE_NULL;
        (void)esp_wifi_get_mode(&mode);
        xEventGroupClearBits(s_wifi_evt, WIFI_CONNECTED_BIT);
        if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
            if (s_last_disc_reason == WIFI_DISC_NO_AP_FOUND) {
                ESP_LOGW(TAG, "WIFI_EVENT_STA_DISCONNECTED reason=%d (no AP found), stop auto-reconnect", s_last_disc_reason);
            } else {
                ESP_LOGW(TAG, "WIFI_EVENT_STA_DISCONNECTED reason=%d -> reconnect", s_last_disc_reason);
                request_reconnect(false);
            }
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP_EVENT_STA_GOT_IP: " IPSTR, IP2STR(&e->ip_info.ip));
        ensure_dns_from_got_ip(e);
        xEventGroupSetBits(s_wifi_evt, WIFI_CONNECTED_BIT);

        if (kEnableUdpEcho && s_udp_task == NULL) {
            xTaskCreate(udp_echo_task, "udp_echo", 4096, NULL, 5, &s_udp_task);
        }
        return;
    }
}

void comm_wifi_start(void)
{
    if (s_started) {
        wifi_mode_t mode = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&mode) == ESP_OK) {
            if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
                ESP_LOGI(TAG, "wifi already in AP/APSTA mode, keep current mode");
                return;
            }
        }
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        esp_wifi_set_ps(WIFI_PS_NONE);
        if (!comm_wifi_is_connected()) (void)comm_wifi_connect_saved();
        return;
    }
    s_started = true;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    s_wifi_evt = xEventGroupCreate();

    s_netif = esp_netif_create_default_wifi_sta();
    (void)s_netif;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA start requested (scan later)");
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (!comm_wifi_is_connected()) (void)comm_wifi_connect_saved();
}

void comm_wifi_stop(void)
{
    if (s_udp_task) {
        vTaskDelete(s_udp_task);
        s_udp_task = NULL;
    }
    esp_wifi_stop();
}

int comm_wifi_scan_top3(CommWifiAp* out, int cap)
{
    int max_out = (cap > 3) ? 3 : cap;
    int count = wifi_scan_list(out, max_out);
    if (count > 0) {
        ESP_LOGI(TAG, "APs top %d:", count);
        for (int i = 0; i < count; i++) {
            ESP_LOGI(TAG, "  #%d %s  RSSI=%d", i + 1, out[i].ssid, out[i].rssi);
        }
    }
    return count;
}

bool comm_wifi_connect_psk(const char* ssid, const char* password)
{
    return set_sta_config_and_connect(ssid, password);
}

bool comm_wifi_is_connected(void)
{
    if (!s_wifi_evt) return false;
    EventBits_t bits = xEventGroupGetBits(s_wifi_evt);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

int comm_wifi_last_disconnect_reason(void)
{
    return s_last_disc_reason;
}

bool comm_wifi_connect_saved(void)
{
    wifi_config_t cfg = {0};
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) return false;
    if (cfg.sta.ssid[0] == 0) return false;

    strncpy(s_target_ssid, (const char*)cfg.sta.ssid, sizeof(s_target_ssid) - 1);
    s_target_ssid[sizeof(s_target_ssid) - 1] = 0;
    s_has_target = true;
    return (esp_wifi_connect() == ESP_OK);
}

bool comm_wifi_get_connected_ssid(char* out_ssid, int cap)
{
    if (!out_ssid || cap < 2) return false;
    out_ssid[0] = 0;

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK || ap.ssid[0] == 0) return false;

    strncpy(out_ssid, (const char*)ap.ssid, (size_t)cap - 1U);
    out_ssid[cap - 1] = 0;
    return true;
}

int comm_wifi_saved_credential_count(void)
{
    SavedWifiCred creds[COMM_WIFI_MAX_CREDENTIALS] = {0};
    int count = 0;
    (void)nvs_load_saved_creds(creds, &count);
    return count;
}

bool comm_wifi_save_credential(const char* ssid, const char* password)
{
    if (!ssid || !ssid[0] || !password) return false;

    SavedWifiCred creds[COMM_WIFI_MAX_CREDENTIALS] = {0};
    int count = 0;
    (void)nvs_load_saved_creds(creds, &count);

    SavedWifiCred n = {0};
    n.valid = 1;
    strncpy(n.ssid, ssid, sizeof(n.ssid) - 1);
    strncpy(n.password, password, sizeof(n.password) - 1);

    SavedWifiCred merged[COMM_WIFI_MAX_CREDENTIALS] = {0};
    int out = 0;
    merged[out++] = n;

    for (int i = 0; i < count && out < COMM_WIFI_MAX_CREDENTIALS; i++) {
        if (strcmp(creds[i].ssid, n.ssid) == 0) continue;
        merged[out++] = creds[i];
    }

    bool ok = nvs_save_saved_creds(merged, out);
    if (ok) ESP_LOGI(TAG, "saved wifi profile: %s (total=%d)", n.ssid, out);
    return ok;
}

bool comm_wifi_connect_saved_any(uint32_t per_profile_timeout_ms, char* out_ssid, int out_cap)
{
    if (!s_started) comm_wifi_start();

    if (out_ssid && out_cap > 0) out_ssid[0] = 0;

    SavedWifiCred creds[COMM_WIFI_MAX_CREDENTIALS] = {0};
    int count = 0;
    if (!nvs_load_saved_creds(creds, &count) || count <= 0) {
        ESP_LOGI(TAG, "no saved wifi profiles");
        return false;
    }

    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    (void)esp_wifi_start();
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    s_scan_in_progress = true;
    esp_err_t scan_err = esp_wifi_scan_start(&scan_cfg, true);
    s_scan_in_progress = false;
    if (scan_err != ESP_OK) {
        ESP_LOGW(TAG, "scan before connect failed: %s", esp_err_to_name(scan_err));
        return false;
    }

    for (int i = 0; i < count; i++) {
        if (!ssid_in_scan_list(creds[i].ssid)) {
            ESP_LOGI(TAG, "skip unseen saved ssid: %s", creds[i].ssid);
            continue;
        }

        ESP_LOGI(TAG, "try saved wifi #%d: %s", i + 1, creds[i].ssid);
        if (s_wifi_evt) xEventGroupClearBits(s_wifi_evt, WIFI_CONNECTED_BIT);
        if (!set_sta_config_and_connect(creds[i].ssid, creds[i].password)) {
            continue;
        }

        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_evt,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(per_profile_timeout_ms));

        if ((bits & WIFI_CONNECTED_BIT) != 0) {
            if (out_ssid && out_cap > 0) {
                strncpy(out_ssid, creds[i].ssid, (size_t)out_cap - 1U);
                out_ssid[out_cap - 1] = 0;
            }
            if (i > 0) {
                (void)comm_wifi_save_credential(creds[i].ssid, creds[i].password);
            }
            return true;
        }
        (void)esp_wifi_disconnect();
        if (s_wifi_evt) xEventGroupClearBits(s_wifi_evt, WIFI_CONNECTED_BIT);
    }

    return false;
}

bool comm_wifi_switch_to_ap_open(const char* ssid)
{
    if (!ssid || !ssid[0]) return false;
    if (!s_started) comm_wifi_start();

    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    (void)s_ap_netif;

    wifi_config_t ap_cfg = {0};
    strncpy((char*)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    (void)esp_wifi_stop();
    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK) return false;
    if (esp_wifi_set_config(WIFI_IF_AP, &ap_cfg) != ESP_OK) return false;
    if (esp_wifi_start() != ESP_OK) return false;
    if (s_wifi_evt) xEventGroupClearBits(s_wifi_evt, WIFI_CONNECTED_BIT);
    return true;
}

bool comm_wifi_switch_to_sta_and_reconnect(void)
{
    if (!s_started) comm_wifi_start();
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
    if (esp_wifi_start() != ESP_OK) return false;
    esp_wifi_set_ps(WIFI_PS_NONE);
    return comm_wifi_connect_saved();
}

bool comm_wifi_switch_to_sta_only(void)
{
    if (!s_started) comm_wifi_start();
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
    if (esp_wifi_start() != ESP_OK) return false;
    esp_wifi_set_ps(WIFI_PS_NONE);
    return true;
}

bool comm_wifi_forget_saved_and_disconnect(void)
{
    if (!s_started) comm_wifi_start();

    (void)esp_wifi_disconnect();
    (void)esp_wifi_stop();

    if (esp_wifi_restore() != ESP_OK) return false;

    nvs_handle_t h;
    if (nvs_open(COMM_WIFI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        for (int i = 0; i < COMM_WIFI_MAX_CREDENTIALS; i++) {
            char key[12];
            snprintf(key, sizeof(key), COMM_WIFI_NVS_SLOT_KEY_FMT, i);
            (void)nvs_erase_key(h, key);
        }
        (void)nvs_commit(h);
        nvs_close(h);
    }

    s_has_target = false;
    s_target_ssid[0] = 0;
    if (s_wifi_evt) xEventGroupClearBits(s_wifi_evt, WIFI_CONNECTED_BIT);

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
    if (esp_wifi_start() != ESP_OK) return false;
    esp_wifi_set_ps(WIFI_PS_NONE);
    return true;
}
