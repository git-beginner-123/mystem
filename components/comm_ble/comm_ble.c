#include "comm_ble.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"

#include "nvs_flash.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"

#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"

static const char* TAG = "comm_ble";
static const char* s_name = "STEM-BLE";

/* gate */
static bool s_inited = false;
static bool s_enabled = false;

/* state */
static volatile CommBleState s_state = kCommBleOff;
static bool s_notify_enabled = false;

static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0;
static uint8_t s_remote_bda[6];

/* handles */
static uint16_t s_service_handle = 0;
static uint16_t s_char_handle = 0;   /* characteristic value handle */
static uint16_t s_descr_handle = 0;  /* CCCD handle */

/* RX cache */
static uint8_t s_last_rx[64];
static volatile int s_last_rx_len = 0;

/* UUIDs (16-bit for easy testing) */
#define SVC_UUID   0x00FF
#define CHR_UUID   0xFF01
#define CCCD_UUID  ESP_GATT_UUID_CHAR_CLIENT_CONFIG

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min        = 0x20,
    .adv_int_max        = 0x40,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x20,
    .max_interval = 0x40,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static uint16_t u16_le(const uint8_t* p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void plus_one(uint8_t* dst, const uint8_t* src, int len)
{
    for (int i = 0; i < len; i++) dst[i] = (uint8_t)(src[i] + 1);
}

static void send_notify_plus_one(void)
{
    if (!s_enabled) return;
    if (s_state != kCommBleConnected) return;
    if (!s_notify_enabled) return;
    if (s_char_handle == 0) return;

    int len = (int)s_last_rx_len;
    if (len <= 0) return;
    if (len > (int)sizeof(s_last_rx)) len = (int)sizeof(s_last_rx);

    uint8_t tx[64];
    plus_one(tx, s_last_rx, len);

    /* notify (confirm=false) */
    esp_ble_gatts_send_indicate(
        s_gatts_if,
        s_conn_id,
        s_char_handle,
        (uint16_t)len,
        tx,
        false
    );
}

static void start_adv_if_allowed(void)
{
    if (!s_enabled) {
        ESP_LOGI(TAG, "adv skipped (disabled)");
        return;
    }
    if (s_state == kCommBleConnected) {
        ESP_LOGI(TAG, "adv skipped (already connected)");
        return;
    }
    esp_err_t err = esp_ble_gap_start_advertising(&s_adv_params);
    ESP_LOGI(TAG, "Advertising start: status=%d", (int)err);
}

static void stop_adv_if_needed(void)
{
    esp_ble_gap_stop_advertising();
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "adv data set");
        /* do NOT auto start advertising here */
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_state = kCommBleAdvertising;
            ESP_LOGI(TAG, "adv start ok");
        } else {
            ESP_LOGE(TAG, "adv start failed, status=%d", param->adv_start_cmpl.status);
            if (s_enabled) s_state = kCommBleIdle;
            else s_state = kCommBleOff;
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "adv stop, status=%d", param->adv_stop_cmpl.status);
        if (s_enabled) s_state = kCommBleIdle;
        else s_state = kCommBleOff;
        break;

    default:
        break;
    }
}

static void create_service(esp_gatt_if_t gatts_if)
{
    esp_gatt_srvc_id_t svc_id;
    memset(&svc_id, 0, sizeof(svc_id));
    svc_id.is_primary = true;
    svc_id.id.inst_id = 0;
    svc_id.id.uuid.len = ESP_UUID_LEN_16;
    svc_id.id.uuid.uuid.uuid16 = SVC_UUID;

    esp_ble_gatts_create_service(gatts_if, &svc_id, 8);
}

static void add_char(void)
{
    esp_bt_uuid_t uuid;
    uuid.len = ESP_UUID_LEN_16;
    uuid.uuid.uuid16 = CHR_UUID;

    esp_gatt_char_prop_t prop = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_NOTIFY;

    esp_attr_value_t val;
    memset(&val, 0, sizeof(val));
    val.attr_max_len = 64;
    val.attr_len = 0;
    val.attr_value = NULL;

    esp_ble_gatts_add_char(
        s_service_handle,
        &uuid,
        ESP_GATT_PERM_WRITE,
        prop,
        &val,
        NULL
    );
}

static void add_cccd(void)
{
    esp_bt_uuid_t uuid;
    uuid.len = ESP_UUID_LEN_16;
    uuid.uuid.uuid16 = CCCD_UUID;

    uint8_t cccd[2] = {0x00, 0x00};
    esp_attr_value_t val;
    memset(&val, 0, sizeof(val));
    val.attr_max_len = 2;
    val.attr_len = 2;
    val.attr_value = cccd;

    esp_ble_gatts_add_char_descr(
        s_service_handle,
        &uuid,
        ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
        &val,
        NULL
    );
}

static void gatts_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                     esp_ble_gatts_cb_param_t* param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        ESP_LOGI(TAG, "gatts reg, app_id=%d", param->reg.app_id);
        s_gatts_if = gatts_if;

        esp_ble_gap_set_device_name(s_name);
        esp_ble_gap_config_adv_data(&s_adv_data);

        create_service(gatts_if);
        break;

    case ESP_GATTS_CREATE_EVT:
        ESP_LOGI(TAG, "service created, handle=%u", (unsigned)param->create.service_handle);
        s_service_handle = param->create.service_handle;
        esp_ble_gatts_start_service(s_service_handle);
        add_char();
        break;

    case ESP_GATTS_ADD_CHAR_EVT:
        ESP_LOGI(TAG, "char added, handle=%u", (unsigned)param->add_char.attr_handle);
        s_char_handle = param->add_char.attr_handle;
        add_cccd();
        break;

    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        ESP_LOGI(TAG, "descr added, handle=%u", (unsigned)param->add_char_descr.attr_handle);
        s_descr_handle = param->add_char_descr.attr_handle;
        break;

    case ESP_GATTS_CONNECT_EVT:
        ESP_LOGI(TAG, "BLE connected, conn_id=%u", (unsigned)param->connect.conn_id);
        s_conn_id = param->connect.conn_id;
        memcpy(s_remote_bda, param->connect.remote_bda, sizeof(s_remote_bda));

        s_notify_enabled = false;
        s_state = kCommBleConnected;

        /* if disabled, close immediately */
        if (!s_enabled) {
            ESP_LOGI(TAG, "close connection (disabled)");
            esp_ble_gatts_close(gatts_if, s_conn_id);
        }
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(TAG, "BLE disconnected, reason=0x%02x", param->disconnect.reason);
        s_notify_enabled = false;
        s_conn_id = 0;

        if (s_enabled) {
            s_state = kCommBleIdle;
            ESP_LOGI(TAG, "restart advertising (enabled)");
            start_adv_if_allowed();
        } else {
            s_state = kCommBleOff;
        }
        break;

    case ESP_GATTS_WRITE_EVT:
        if (param->write.is_prep) break;

        /* gate: ignore all writes when disabled */
        if (!s_enabled) {
            ESP_LOGI(TAG, "WRITE ignored (disabled) handle=%u len=%u",
                     (unsigned)param->write.handle, (unsigned)param->write.len);
            break;
        }

        ESP_LOGI(TAG, "WRITE handle=%u len=%u",
                 (unsigned)param->write.handle, (unsigned)param->write.len);

        /* CCCD write: enable/disable notify */
        if (s_descr_handle != 0 &&
            param->write.handle == s_descr_handle &&
            param->write.len >= 2) {

            uint16_t v = u16_le(param->write.value);
            s_notify_enabled = (v == 0x0001);
            ESP_LOGI(TAG, "CCCD=%u notify=%d", (unsigned)v, (int)s_notify_enabled);
            break;
        }

        /* characteristic value write */
        if (s_char_handle != 0 && param->write.handle == s_char_handle) {
            int len = (int)param->write.len;
            if (len > (int)sizeof(s_last_rx)) len = (int)sizeof(s_last_rx);

            memcpy(s_last_rx, param->write.value, len);
            s_last_rx_len = len;

            /* debug RX */
            char line[128];
            line[0] = 0;
            for (int i = 0; i < len && i < 16; i++) {
                char tmp[8];
                snprintf(tmp, sizeof(tmp), "%02X ", s_last_rx[i]);
                strncat(line, tmp, sizeof(line) - strlen(line) - 1);
            }
            ESP_LOGI(TAG, "RX: %s", line);

            if (s_notify_enabled) {
                send_notify_plus_one();
            }
        } else {
            ESP_LOGW(TAG, "WRITE to unknown handle=%u (char=%u descr=%u)",
                     (unsigned)param->write.handle,
                     (unsigned)s_char_handle,
                     (unsigned)s_descr_handle);
        }
        break;

    default:
        break;
    }
}

void CommBle_InitOnce(void)
{
    if (s_inited) return;
    s_inited = true;

    /* NVS required by Bluedroid */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        (void)nvs_flash_init();
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) ESP_LOGW(TAG, "controller init err=%d", (int)err);

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) ESP_LOGW(TAG, "controller enable err=%d", (int)err);

    err = esp_bluedroid_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "bluedroid init err=%d", (int)err);

    err = esp_bluedroid_enable();
    if (err != ESP_OK) ESP_LOGW(TAG, "bluedroid enable err=%d", (int)err);

    esp_ble_gap_register_callback(gap_cb);
    esp_ble_gatts_register_callback(gatts_cb);
    esp_ble_gatts_app_register(0);

    /* default: disabled */
    s_enabled = false;
    s_state = kCommBleOff;
    s_notify_enabled = false;
    s_last_rx_len = 0;
}

void CommBle_Enable(bool en)
{
    if (!s_inited) CommBle_InitOnce();
    if (s_enabled == en) return;

    s_enabled = en;

    if (!s_enabled) {
        ESP_LOGI(TAG, "disable: stop adv + disconnect + clear rx");
        stop_adv_if_needed();

        if (s_state == kCommBleConnected && s_gatts_if != ESP_GATT_IF_NONE) {
            esp_ble_gatts_close(s_gatts_if, s_conn_id);
        }

        s_notify_enabled = false;
        s_conn_id = 0;
        s_last_rx_len = 0;
        s_state = kCommBleOff;
        return;
    }

    ESP_LOGI(TAG, "enable: start advertising");
    s_state = kCommBleIdle;
    start_adv_if_allowed();
}

bool CommBle_IsEnabled(void)
{
    return s_enabled;
}

CommBleState CommBle_GetState(void)
{
    return s_state;
}

const char* CommBle_GetName(void)
{
    return s_name;
}

void CommBle_GetAddrStr(char* out, int len)
{
    if (!out || len < 18) return;

    const uint8_t* bda = esp_bt_dev_get_address();
    if (!bda) {
        snprintf(out, len, "00:00:00:00:00:00");
        return;
    }

    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

int CommBle_GetLastRx(uint8_t* out, int max_len)
{
    int len = (int)s_last_rx_len;
    if (len > max_len) len = max_len;
    if (len > 0 && out) memcpy(out, s_last_rx, len);
    return len;
}

int CommBle_GetLastRxLen(void)
{
    return (int)s_last_rx_len;
}

void CommBle_ClearLastRx(void)
{
    s_last_rx_len = 0;
}
