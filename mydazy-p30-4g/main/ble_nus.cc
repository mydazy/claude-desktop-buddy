#include "ble_nus.h"
#include "data.h"
#include "display.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <nvs_flash.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/ble_uuid.h>
#include <host/util/util.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_store.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

static const char *TAG = "nus";

// ─── NUS UUIDs (NimBLE stores 128-bit UUIDs little-endian) ───────────────
// Service     6e400001-b5a3-f393-e0a9-e50e24dcca9e
// RX (write)  6e400002-...
// TX (notify) 6e400003-...
static const ble_uuid128_t NUS_SVC_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t NUS_RX_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t NUS_TX_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

// ─── Connection state (single peer, peripheral role) ─────────────────────
static uint16_t conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
static uint16_t tx_attr_handle_ = 0;
static bool tx_subscribed_ = false;
static bool encrypted_ = false;
static char device_name_[20];
static SemaphoreHandle_t tx_lock_;

// Ring-buffered RX line accumulator. Desktop sends '\n'-delimited JSON;
// NimBLE delivers writes as MTU-sized chunks. Accumulate until '\n' and
// dispatch the full line to data_apply_line().
static char rx_buf_[1024];
static size_t rx_len_ = 0;

// ─── Forward decls ───────────────────────────────────────────────────────
static int gap_event_cb(struct ble_gap_event *event, void *arg);
static void start_advertising();

// ─── GATT access callbacks ───────────────────────────────────────────────
static int rx_write(uint16_t conn_handle, uint16_t attr_handle,
                    struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    struct os_mbuf *om = ctxt->om;
    while (om) {
        const uint8_t *p = om->om_data;
        for (uint16_t i = 0; i < om->om_len; i++) {
            char c = (char)p[i];
            if (c == '\n' || c == '\r') {
                if (rx_len_ > 0) {
                    rx_buf_[rx_len_] = 0;
                    if (rx_buf_[0] == '{') data_apply_line(rx_buf_);
                    rx_len_ = 0;
                }
            } else if (rx_len_ < sizeof(rx_buf_) - 1) {
                rx_buf_[rx_len_++] = c;
            } else {
                rx_len_ = 0;     // line too long; drop & resync at next \n
            }
        }
        om = SLIST_NEXT(om, om_next);
    }
    return 0;
}

static int tx_access(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;       // notify-only
}

// ─── Service definition ──────────────────────────────────────────────────
// Encrypted-required on both characteristics; matches REFERENCE.md
// "Security and pairing".
static const struct ble_gatt_chr_def NUS_CHARS[] = {
    {
        .uuid = &NUS_RX_UUID.u,
        .access_cb = rx_write,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP
               | BLE_GATT_CHR_F_WRITE_ENC,
    },
    {
        .uuid = &NUS_TX_UUID.u,
        .access_cb = tx_access,
        .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
        .val_handle = &tx_attr_handle_,
    },
    { 0 }
};

static const struct ble_gatt_svc_def NUS_SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SVC_UUID.u,
        .characteristics = NUS_CHARS,
    },
    { 0 }
};

// ─── GAP advertising ─────────────────────────────────────────────────────
static void compute_device_name() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(device_name_, sizeof(device_name_), "%s-%02X%02X",
             CONFIG_CLAUDE_BUDDY_BRAND_NAME, mac[4], mac[5]);
}

static void start_advertising() {
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN,
        .itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX,
    };

    // adv payload: flags + 128-bit service UUID (fits in 31 bytes alone).
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.uuids128 = (ble_uuid128_t *)&NUS_SVC_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc) ESP_LOGW(TAG, "adv_set_fields rc=%d", rc);

    // Scan response carries the full "Claude-XXXX" name so the desktop
    // picker can filter on the prefix.
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)device_name_;
    rsp.name_len = strlen(device_name_);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc) ESP_LOGE(TAG, "adv_rsp_set_fields rc=%d", rc);

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "advertising as %s", device_name_);
        display_set_link(DISPLAY_LINK_ADVERTISING);
        display_set_device_name(device_name_);
    } else {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
    }
}

// ─── GAP event handler ───────────────────────────────────────────────────
static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                conn_handle_ = event->connect.conn_handle;
                encrypted_ = false;
                ESP_LOGI(TAG, "connected handle=%u", conn_handle_);
                ble_gap_security_initiate(conn_handle_);
            } else {
                ESP_LOGW(TAG, "connect fail status=%d", event->connect.status);
                start_advertising();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
            conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
            tx_subscribed_ = false;
            encrypted_ = false;
            display_set_link(DISPLAY_LINK_ADVERTISING);
            display_set_passkey(0);
            data_on_disconnect();
            start_advertising();
            return 0;

        case BLE_GAP_EVENT_PASSKEY_ACTION: {
            if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
                struct ble_sm_io io = {0};
                io.action = BLE_SM_IOACT_DISP;
                io.passkey = (uint32_t)(esp_random() % 1000000);
                ble_sm_inject_io(event->passkey.conn_handle, &io);
                ESP_LOGI(TAG, "passkey: %06u", (unsigned)io.passkey);
                display_set_passkey(io.passkey);
                display_set_link(DISPLAY_LINK_PAIRING);
            }
            return 0;
        }

        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(TAG, "enc_change status=%d", event->enc_change.status);
            if (event->enc_change.status == 0) {
                encrypted_ = true;
                display_set_passkey(0);
                display_set_link(DISPLAY_LINK_PAIRED_IDLE);
            }
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == tx_attr_handle_) {
                tx_subscribed_ = event->subscribe.cur_notify;
                ESP_LOGI(TAG, "TX subscribe=%d", tx_subscribed_);
            }
            return 0;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU=%u", event->mtu.value);
            return 0;
    }
    return 0;
}

// ─── Public API ──────────────────────────────────────────────────────────
void ble_nus_send_line(const char *line) {
    if (!line) return;
    if (conn_handle_ == BLE_HS_CONN_HANDLE_NONE || !tx_subscribed_) return;
    if (xSemaphoreTake(tx_lock_, pdMS_TO_TICKS(100)) != pdTRUE) return;

    size_t n = strlen(line);
    size_t total = n + 1;                            // append '\n'
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) { xSemaphoreGive(tx_lock_); return; }
    memcpy(buf, line, n);
    buf[n] = '\n';

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, total);
    free(buf);
    if (!om) { xSemaphoreGive(tx_lock_); return; }

    int rc = ble_gatts_notify_custom(conn_handle_, tx_attr_handle_, om);
    if (rc) ESP_LOGW(TAG, "notify rc=%d", rc);
    xSemaphoreGive(tx_lock_);
}

bool ble_nus_is_connected() {
    return conn_handle_ != BLE_HS_CONN_HANDLE_NONE;
}

void ble_nus_clear_bonds() {
    int rc = ble_store_clear();
    ESP_LOGI(TAG, "ble_store_clear rc=%d", rc);
    if (conn_handle_ != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn_handle_, BLE_ERR_REM_USER_CONN_TERM);
    }
}

// ─── Host bring-up ───────────────────────────────────────────────────────
static void on_sync() {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc) ESP_LOGE(TAG, "ensure_addr rc=%d", rc);
    start_advertising();
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "host reset reason=%d", reason);
}

static void host_task(void *param) {
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_nus_init() {
    tx_lock_ = xSemaphoreCreateMutex();
    compute_device_name();

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init err=%d", err);
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // LE Secure Connections + DisplayOnly IO + bonding + MITM. The desktop
    // user types the 6-digit passkey shown on the LCD.
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(NUS_SVCS);
    if (rc) ESP_LOGE(TAG, "gatts_count_cfg rc=%d", rc);
    rc = ble_gatts_add_svcs(NUS_SVCS);
    if (rc) ESP_LOGE(TAG, "gatts_add_svcs rc=%d", rc);

    ble_svc_gap_device_name_set(device_name_);

    nimble_port_freertos_init(host_task);
}
