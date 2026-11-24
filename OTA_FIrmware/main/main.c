/*
 * Allreal_CPR - ESP32-C3 BLE OTA (ESP-IDF 5.x, NimBLE)
 *
 * GATT service (custom):
 *   Control char (WRITE | NOTIFY)
 *       write 0x01 + u32 LE size  -> START
 *       write 0x02                -> END (verify, set boot partition, reboot)
 *       write 0x03                -> ABORT
 *       notify [status, u32 LE received]
 *            status: 0x00 READY, 0x02 DONE, 0xFF ERROR
 *   Data char (WRITE | WRITE_NO_RSP)
 *       raw firmware bytes, sequential chunks (<= MTU-3)
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* =============================================================================
 * MANIKIN IDENTITY & SECURITY CONFIGURATION ("Manikin_10X" + High Security)
 * =============================================================================
 * Each training manikin is uniquely identified in OTA mode (e.g. Manikin_101, Manikin_103).
 * Secure Pairing:
 *   - LE Secure Connections (ECDH AES-128)
 *   - MITM Protection enabled
 *   - 6-digit Passkey authentication enforced on GATT Control & Data chars
 * ============================================================================= */
#define CPR_DEVICE_PREFIX "Manikin_"

#ifndef CONFIG_CPR_MANIKIN_ID
#define CONFIG_CPR_MANIKIN_ID "103" /* Manikin 103 */
#endif

#ifndef CONFIG_CPR_OTA_PASSKEY
#define CONFIG_CPR_OTA_PASSKEY 123456 /* Default 6-digit secure passkey */
#endif

static char s_device_name[32] = "Manikin_103";
static uint8_t s_device_mac[6] = {0};
static uint32_t s_passkey = CONFIG_CPR_OTA_PASSKEY;
static const char *TAG = "CPR_OTA";

/* ---- UUIDs (little-endian byte order for NimBLE) ----
 * Service : 4fafc201-1fb5-459e-8fcc-c5c9c331914b
 * Control : beb5483e-36e1-4688-b7f5-ea07361b26a8
 * Data    : 1c95d5e3-d8f7-413a-bf3d-7a2e5d7be87e
 */
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x4b, 0x91, 0x31, 0xc3, 0xc9, 0xc5, 0xcc, 0x8f,
    0x9e, 0x45, 0xb5, 0x1f, 0x01, 0xc2, 0xaf, 0x4f);
static const ble_uuid128_t ctrl_uuid = BLE_UUID128_INIT(
    0xa8, 0x26, 0x1b, 0x36, 0x07, 0xea, 0xf5, 0xb7,
    0x88, 0x46, 0xe1, 0x36, 0x3e, 0x48, 0xb5, 0xbe);
static const ble_uuid128_t data_uuid = BLE_UUID128_INIT(
    0x7e, 0xe8, 0x7b, 0x5d, 0x2e, 0x7a, 0x3d, 0xbf,
    0x3a, 0x41, 0xf7, 0xd8, 0xe3, 0xd5, 0x95, 0x1c);

enum { CMD_START = 0x01, CMD_END = 0x02, CMD_ABORT = 0x03, CMD_SET_NAME = 0x04, CMD_GET_INFO = 0x05 };
enum { ST_READY = 0x00, ST_DONE = 0x02, ST_ERROR = 0xFF };

static uint8_t own_addr_type;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t ctrl_val_handle;

static const esp_partition_t *ota_part;
static esp_ota_handle_t ota_handle;
static bool ota_active;
static uint32_t ota_total, ota_received;

static void start_advertising(void);

/* ------------------------------------------------------------------ */
static void init_device_identity(void)
{
    /* 1. Read hardware factory Bluetooth MAC address from eFuse */
    esp_read_mac(s_device_mac, ESP_MAC_BT);
    ESP_LOGI(TAG, "Factory BT MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             s_device_mac[0], s_device_mac[1], s_device_mac[2],
             s_device_mac[3], s_device_mac[4], s_device_mac[5]);

    /* 2. Check NVS for a previously saved unique Manikin Name or ID */
    nvs_handle_t nvs;
    bool nvs_loaded = false;
    if (nvs_open("cpr_cfg", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_device_name);
        if (nvs_get_str(nvs, "dev_name", s_device_name, &len) == ESP_OK && len > 1) {
            nvs_loaded = true;
            ESP_LOGI(TAG, "Loaded unique Manikin name from NVS: %s", s_device_name);
        } else if (nvs_get_str(nvs, "manikin_id", s_device_name, &len) == ESP_OK && len > 1) {
            char temp_id[16];
            strncpy(temp_id, s_device_name, sizeof(temp_id) - 1);
            temp_id[sizeof(temp_id) - 1] = '\0';
            snprintf(s_device_name, sizeof(s_device_name), "%s%s", CPR_DEVICE_PREFIX, temp_id);
            nvs_loaded = true;
            ESP_LOGI(TAG, "Loaded manikin_id from NVS: %s -> %s", temp_id, s_device_name);
        }

        uint32_t nvs_passkey = 0;
        if (nvs_get_u32(nvs, "passkey", &nvs_passkey) == ESP_OK && nvs_passkey <= 999999) {
            s_passkey = nvs_passkey;
            ESP_LOGI(TAG, "Loaded custom pairing passkey from NVS: %06lu", (unsigned long)s_passkey);
        }
        nvs_close(nvs);
    }

    /* 3. If no NVS configuration found, construct CPR_10X */
    if (!nvs_loaded) {
#if defined(CONFIG_CPR_MANIKIN_NAME)
        /* Explicit name override defined at compile-time */
        strncpy(s_device_name, CONFIG_CPR_MANIKIN_NAME, sizeof(s_device_name) - 1);
        s_device_name[sizeof(s_device_name) - 1] = '\0';
#elif defined(CONFIG_CPR_MANIKIN_ID)
        if (strcmp(CONFIG_CPR_MANIKIN_ID, "AUTO") == 0) {
            /* Auto-derive X from MAC: gives Manikin_101 through Manikin_109 */
            uint8_t unit_digit = (s_device_mac[5] % 9) + 1;
            snprintf(s_device_name, sizeof(s_device_name), "Manikin_10%u", (unsigned int)unit_digit);
        } else {
            /* Explicit ID defined (e.g. "103" -> "Manikin_103", "101" -> "Manikin_101") */
            snprintf(s_device_name, sizeof(s_device_name), "%s%s", CPR_DEVICE_PREFIX, CONFIG_CPR_MANIKIN_ID);
        }
#else
        /* Auto-derive X (1..9) from hardware MAC byte 5 */
        uint8_t unit_digit = (s_device_mac[5] % 9) + 1;
        snprintf(s_device_name, sizeof(s_device_name), "Manikin_10%u", (unsigned int)unit_digit);
#endif
        ESP_LOGI(TAG, "Assigned default unique Manikin name: %s", s_device_name);
    }

    ESP_LOGI(TAG, "Manikin Active Identity: %s [HW ID: %02X:%02X:%02X]",
             s_device_name, s_device_mac[3], s_device_mac[4], s_device_mac[5]);
}

/* ------------------------------------------------------------------ */
static void notify_status(uint8_t status)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    uint8_t buf[5] = { status,
        (uint8_t)ota_received, (uint8_t)(ota_received >> 8),
        (uint8_t)(ota_received >> 16), (uint8_t)(ota_received >> 24) };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, sizeof(buf));
    if (om) ble_gatts_notify_custom(conn_handle, ctrl_val_handle, om);
}

static void notify_info(void)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    uint8_t name_len = (uint8_t)strlen(s_device_name);
    if (name_len > 24) name_len = 24;
    uint8_t buf[32];
    buf[0] = CMD_GET_INFO;
    memcpy(&buf[1], s_device_mac, 6);
    buf[7] = name_len;
    memcpy(&buf[8], s_device_name, name_len);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, 8 + name_len);
    if (om) ble_gatts_notify_custom(conn_handle, ctrl_val_handle, om);
}

static void ota_abort(void)
{
    if (ota_active) {
        esp_ota_abort(ota_handle);
        ota_active = false;
    }
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));   /* let the DONE notify go out */
    esp_restart();
}

/* ------------------------------------------------------------------ */
static int ctrl_access(uint16_t ch, uint16_t ah,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint8_t buf[36];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 1 || len > sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    os_mbuf_copydata(ctxt->om, 0, len, buf);

    switch (buf[0]) {
    case CMD_START: {
        if (len < 5) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        ota_abort();
        ota_total = buf[1] | (buf[2] << 8) | (buf[3] << 16) | ((uint32_t)buf[4] << 24);
        ota_received = 0;
        ota_part = esp_ota_get_next_update_partition(NULL);
        if (!ota_part || ota_total == 0 || ota_total > ota_part->size) {
            ESP_LOGE(TAG, "Bad size/partition (%lu)", (unsigned long)ota_total);
            notify_status(ST_ERROR);
            return 0;
        }
        esp_err_t err = esp_ota_begin(ota_part, OTA_SIZE_UNKNOWN, &ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
            notify_status(ST_ERROR);
            return 0;
        }
        ota_active = true;
        ESP_LOGI(TAG, "OTA start: %lu bytes -> %s", (unsigned long)ota_total, ota_part->label);
        notify_status(ST_READY);
        return 0;
    }
    case CMD_END: {
        if (!ota_active || ota_received != ota_total) {
            ESP_LOGE(TAG, "END with %lu/%lu", (unsigned long)ota_received, (unsigned long)ota_total);
            ota_abort();
            notify_status(ST_ERROR);
            return 0;
        }
        ota_active = false;
        esp_err_t err = esp_ota_end(ota_handle);          /* validates image */
        if (err == ESP_OK) err = esp_ota_set_boot_partition(ota_part);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA finalize failed: %s", esp_err_to_name(err));
            notify_status(ST_ERROR);
            return 0;
        }
        ESP_LOGI(TAG, "OTA OK, rebooting");
        notify_status(ST_DONE);
        xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
        return 0;
    }
    case CMD_ABORT:
        ota_abort();
        return 0;
    case CMD_SET_NAME: {
        /* Set and persist unique Manikin name ("CPR_10X") into NVS */
        if (len < 2) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        char new_name[32] = {0};
        uint16_t name_bytes = len - 1;
        if (name_bytes >= sizeof(new_name)) name_bytes = sizeof(new_name) - 1;
        memcpy(new_name, &buf[1], name_bytes);
        new_name[name_bytes] = '\0';

        strncpy(s_device_name, new_name, sizeof(s_device_name) - 1);
        s_device_name[sizeof(s_device_name) - 1] = '\0';
        ble_svc_gap_device_name_set(s_device_name);

        nvs_handle_t nvs;
        if (nvs_open("cpr_cfg", NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_str(nvs, "dev_name", s_device_name);
            nvs_commit(nvs);
            nvs_close(nvs);
            ESP_LOGI(TAG, "Updated persistent Manikin name in NVS: %s", s_device_name);
        }
        notify_status(ST_READY);
        return 0;
    }
    case CMD_GET_INFO:
        notify_info();
        return 0;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static int data_access(uint16_t ch, uint16_t ah,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (!ota_active) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;

    static uint8_t chunk[512];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > sizeof(chunk) || ota_received + len > ota_total) {
        ota_abort();
        notify_status(ST_ERROR);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    os_mbuf_copydata(ctxt->om, 0, len, chunk);

    esp_err_t err = esp_ota_write(ota_handle, chunk, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
        ota_abort();
        notify_status(ST_ERROR);
        return BLE_ATT_ERR_UNLIKELY;
    }
    ota_received += len;
    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &ctrl_uuid.u, .access_cb = ctrl_access,
              .val_handle = &ctrl_val_handle,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY |
                       BLE_GATT_CHR_F_WRITE_AUTHEN | BLE_GATT_CHR_F_WRITE_ENC },
            { .uuid = &data_uuid.u, .access_cb = data_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                       BLE_GATT_CHR_F_WRITE_AUTHEN | BLE_GATT_CHR_F_WRITE_ENC },
            { 0 }
        },
    },
    { 0 }
};

/* ------------------------------------------------------------------ */
static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Connected (handle=%d)", conn_handle);
        } else {
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected (reason=%d)", event->disconnect.reason);
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ota_abort();
        start_advertising();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU = %d", event->mtu.value);
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io pkey = {0};
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            pkey.action = event->passkey.params.action;
            pkey.passkey = s_passkey;
            ESP_LOGW(TAG, "***************************************************");
            ESP_LOGW(TAG, "*** SECURE PAIRING PASSKEY: %06lu ***", (unsigned long)s_passkey);
            ESP_LOGW(TAG, "***************************************************");
            int rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            if (rc != 0) {
                ESP_LOGE(TAG, "ble_sm_inject_io failed: %d", rc);
            }
        } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            ESP_LOGI(TAG, "Passkey Numeric Comparison: %06lu", (unsigned long)event->passkey.params.numcmp);
            pkey.action = event->passkey.params.action;
            pkey.numcmp_accept = 1;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        }
        break;
    }
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                ESP_LOGI(TAG, "Secure Encryption / Pairing Complete (Bonded: %d, Authen: %d, Encrypted: %d)",
                         desc.sec_state.bonded,
                         desc.sec_state.authenticated,
                         desc.sec_state.encrypted);
            }
        } else {
            ESP_LOGE(TAG, "Authentication / Encryption FAILED: status=%d", event->enc_change.status);
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        break;
    }
    return 0;
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.name = (uint8_t *)s_device_name;
    adv.name_len = strlen(s_device_name);
    adv.name_is_complete = 1;
    ble_gap_adv_set_fields(&adv);

    /* 128-bit UUID goes in the scan response (adv packet is too small) */
    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)&svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv start rc=%d", rc);
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &own_addr_type);
    start_advertising();
    ESP_LOGI(TAG, "Advertising as %s (HW MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
             s_device_name,
             s_device_mac[0], s_device_mac[1], s_device_mac[2],
             s_device_mac[3], s_device_mac[4], s_device_mac[5]);
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Rollback: confirm this image is good (do this after your own self-checks) */
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
    }

    /* Initialize unique Manikin identity (CPR_10X) */
    init_device_identity();

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_att_set_preferred_mtu(512);

    /* Configure BLE Security Manager (High Security with 6-digit Passkey) */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1; /* LE Secure Connections (ECDH encryption) */
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_device_name);
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));

    nimble_port_freertos_init(host_task);
}
