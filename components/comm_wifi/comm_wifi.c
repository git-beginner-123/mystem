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

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"

static const char *TAG = "comm_wifi";

static EventGroupHandle_t s_wifi_evt;
static const int WIFI_CONNECTED_BIT = BIT0;

static TaskHandle_t s_udp_task = NULL;
static esp_netif_t *s_netif = NULL;
static bool s_has_target = false;
static char s_target_ssid[33];

static int cmp_rssi_desc(const void* a, const void* b)
{
    const wifi_ap_record_t* pa = (const wifi_ap_record_t*)a;
    const wifi_ap_record_t* pb = (const wifi_ap_record_t*)b;
    return (pb->rssi - pa->rssi);
}

static int wifi_scan_list(CommWifiAp* out, int cap)
{
    if (!out || cap <= 0) return 0;

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true /* block */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t ap_num = 0;
    if (esp_wifi_scan_get_ap_num(&ap_num) != ESP_OK || ap_num == 0) {
        ESP_LOGI(TAG, "scan: no APs found");
        return 0;
    }

    wifi_ap_record_t *aps = (wifi_ap_record_t *)calloc(ap_num, sizeof(*aps));
    if (!aps) {
        ESP_LOGW(TAG, "scan: out of memory");
        return 0;
    }

    int count = 0;
    if (esp_wifi_scan_get_ap_records(&ap_num, aps) == ESP_OK) {
        ESP_LOGI(TAG, "scan: %u APs", (unsigned)ap_num);
        for (uint16_t i = 0; i < ap_num; i++) {
            const char *auth = (aps[i].authmode == WIFI_AUTH_OPEN) ? "OPEN" : "SEC";
            ESP_LOGI(TAG, "  %02u) %s  RSSI=%d  %s", i + 1, (char *)aps[i].ssid, aps[i].rssi, auth);
        }

        int count_all = 0;
        for (uint16_t i = 0; i < ap_num; i++) {
            if (aps[i].ssid[0]) {
                aps[count_all++] = aps[i];
            }
        }

        if (count_all > 0) {
            qsort(aps, (size_t)count_all, sizeof(*aps), cmp_rssi_desc);
            int pick = (count_all > cap) ? cap : count_all;
            for (int i = 0; i < pick; i++) {
                strncpy(out[i].ssid, (char *)aps[i].ssid, sizeof(out[i].ssid) - 1);
                out[i].ssid[sizeof(out[i].ssid) - 1] = 0;
                out[i].rssi = aps[i].rssi;
            }
            count = pick;
        }
    }

    free(aps);
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
        ESP_LOGW(TAG, "WIFI_EVENT_STA_DISCONNECTED -> reconnect");
        xEventGroupClearBits(s_wifi_evt, WIFI_CONNECTED_BIT);
        if (s_has_target && s_target_ssid[0]) {
            esp_wifi_connect();
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP_EVENT_STA_GOT_IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_evt, WIFI_CONNECTED_BIT);

        if (s_udp_task == NULL) {
            xTaskCreate(udp_echo_task, "udp_echo", 4096, NULL, 5, &s_udp_task);
        }
        return;
    }
}
// (removed old alternative handler and single-pick scan helper)

void comm_wifi_start(void)
{
    static bool started = false;
    if (started) return;
    started = true;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());

    // If your project already created the default loop, this returns ESP_ERR_INVALID_STATE.
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    s_wifi_evt = xEventGroupCreate();

    s_netif = esp_netif_create_default_wifi_sta();
    (void)s_netif;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA start requested (scan later)");
    esp_wifi_set_ps(WIFI_PS_NONE);

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
    if (!ssid || !ssid[0] || !password) return false;

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) return false;

    strncpy(s_target_ssid, ssid, sizeof(s_target_ssid) - 1);
    s_has_target = true;
    return (esp_wifi_connect() == ESP_OK);
}

bool comm_wifi_is_connected(void)
{
    if (!s_wifi_evt) return false;
    EventBits_t bits = xEventGroupGetBits(s_wifi_evt);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}
