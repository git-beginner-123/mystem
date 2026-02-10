#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ssid[33];
    int rssi;
} CommWifiAp;

void comm_wifi_start(void);
void comm_wifi_stop(void);

int  comm_wifi_scan_top3(CommWifiAp* out, int cap);
bool comm_wifi_connect_psk(const char* ssid, const char* password);
bool comm_wifi_is_connected(void);

#ifdef __cplusplus
}
#endif
