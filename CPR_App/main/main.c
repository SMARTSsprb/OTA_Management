/*
 * CPR Manikin Application Firmware (ESP-IDF 5.x)
 * Target: ESP32-C3 Super Mini
 * Partition: ota_1 (Normal Training Mode)
 *
 * Identity: Advertises as "CPR_103" (or configured ID)
 * Services:
 *   - Nordic UART Service (NUS) for Real-Time Telemetry & App Control
 *   - Battery Service (0x180F)
 *
 * Maintenance / OTA Switching:
 *   - Wireless: Write "@OTA" to NUS RX -> switches boot partition to ota_0 (Manikin_103) & reboots.
 *   - Hardware: Hold GPIO 9 (Boot button) for 3 seconds -> reboots to ota_0.
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "cpr_protocol.h"

static const char *TAG = "CPR_APP";

#define DEFAULT_MANIKIN_NAME "CPR_103"
#define BOOT_BUTTON_GPIO      GPIO_NUM_9
#define IR1_PIN               GPIO_NUM_3
#define IR2_PIN               GPIO_NUM_4
#define MOTOR_PIN             GPIO_NUM_6

static char s_device_name[32] = DEFAULT_MANIKIN_NAME;
static uint8_t s_device_mac[6] = {0};
static uint8_t own_addr_type;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t nus_tx_val_handle;
static bool s_tx_notify_enabled = false;
static uint16_t s_packet_id = 0;

/* ---- Nordic UART Service (NUS) UUIDs ----
 * Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 * RX     : 6E400002-B5A3-F393-E0A9-E50E24DCCA9E (Write)
 * TX     : 6E400003-B5A3-F393-E0A9-E50E24DCCA9E (Notify)
 */
static const ble_uuid128_t nus_svc_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);

static const ble_uuid128_t nus_rx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

static const ble_uuid128_t nus_tx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static void start_advertising(void);

static void reboot_to_ota_mode(void)
{
    ESP_LOGW(TAG, ">>> REBOOTING TO OTA MAINTENANCE MODE (ota_0: Manikin_103) <<<");
    const esp_partition_t *ota_0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (ota_0) {
        esp_err_t err = esp_ota_set_boot_partition(ota_0);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Boot partition set to ota_0 successfully. Restarting...");
        } else {
            ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "ota_0 partition not found!");
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void send_nus_tx(const void *data, uint16_t len)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_tx_notify_enabled) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om) {
        ble_gatts_notify_custom(conn_handle, nus_tx_val_handle, om);
    }
}

static int nus_chr_access(uint16_t conn_h, uint16_t attr_h,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        char rx_buf[128];
        if (len >= sizeof(rx_buf)) len = sizeof(rx_buf) - 1;
        os_mbuf_copydata(ctxt->om, 0, len, rx_buf);
        rx_buf[len] = '\0';

        ESP_LOGI(TAG, "NUS RX received (%d bytes): %s", len, rx_buf);

        /* Check for OTA maintenance mode command */
        if (strncmp(rx_buf, "@OTA", 4) == 0 ||
            strncmp(rx_buf, "@DFU", 4) == 0 ||
            strncmp(rx_buf, "@UPDATE", 7) == 0) {
            const char *resp = "@REBOOTING_OTA\n";
            send_nus_tx(resp, strlen(resp));
            vTaskDelay(pdMS_TO_TICKS(200));
            reboot_to_ota_mode();
            return 0;
        }

        if (strncmp(rx_buf, "@GET", 4) == 0) {
            char resp[64];
            snprintf(resp, sizeof(resp), "@STATUS:NORMAL:%s\n", s_device_name);
            send_nus_tx(resp, strlen(resp));
            return 0;
        }

        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &nus_rx_uuid.u,
                .access_cb = nus_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &nus_tx_uuid.u,
                .access_cb = nus_chr_access,
                .val_handle = &nus_tx_val_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ,
            },
            { 0 }
        },
    },
    { 0 }
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "BLE connected, status=%d handle=%d",
                 event->connect.status, event->connect.conn_handle);
        if (event->connect.status == 0) {
            conn_handle = event->connect.conn_handle;
        } else {
            conn_handle = BLE_HS_CONN_HANDLE_NONE;
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected, reason=%d", event->disconnect.reason);
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_tx_notify_enabled = false;
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == nus_tx_val_handle) {
            s_tx_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "NUS TX notifications %s", s_tx_notify_enabled ? "ENABLED" : "DISABLED");
        }
        return 0;

    default:
        return 0;
    }
}

static void start_advertising(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_device_name;
    fields.name_len = strlen(s_device_name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields error: %d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start error: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "Advertising active as: %s", s_device_name);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    start_advertising();
}

static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void cpr_telemetry_task(void *param)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE && s_tx_notify_enabled) {
            AnnouncePacket ann;
            memset(&ann, 0, sizeof(ann));
            ann.magic[0] = PACKET_MAGIC_0;
            ann.magic[1] = PACKET_MAGIC_1;
            ann.version = PROTOCOL_VER;
            ann.type = PTYPE_ANNOUNCE;
            ann.timestamp = (uint32_t)(esp_timer_get_time() / 1000ULL);
            ann.id = s_packet_id++;
            ann.depth = 100; /* Battery % */
            ann.deviceID[0] = s_device_mac[3];
            ann.deviceID[1] = s_device_mac[4];
            ann.deviceID[2] = s_device_mac[5];
            strncpy(ann.manikinName, "103", sizeof(ann.manikinName));
            ann.manikinType = MANIKIN_TYPE_DEFAULT;
            ann.checksum = cpr_calc_checksum((const uint8_t *)&ann, sizeof(ann) - 1u);

            send_nus_tx(&ann, sizeof(ann));
        }

        /* Check hardware Boot button (GPIO 9, active-low) */
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            int hold_cnt = 0;
            while (gpio_get_level(BOOT_BUTTON_GPIO) == 0 && hold_cnt < 30) {
                vTaskDelay(pdMS_TO_TICKS(100));
                hold_cnt++;
            }
            if (hold_cnt >= 30) {
                ESP_LOGW(TAG, "Physical Boot button held for 3s -> switching to OTA mode!");
                reboot_to_ota_mode();
            }
        }
    }
}

void app_main(void)
{
    /* 1. CRITICAL: Cancel bootloader rollback to mark this partition healthy */
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "Application booted from ota_1! Rollback cancelled.");

    /* 2. Configure hardware pins */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    /* 3. NVS Initialization */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* 4. Obtain MAC & Device Name */
    esp_read_mac(s_device_mac, ESP_MAC_BT);
    snprintf(s_device_name, sizeof(s_device_name), "CPR_103");
    ESP_LOGI(TAG, "Manikin Active Training Identity: %s [MAC: %02X:%02X:%02X:%02X:%02X:%02X]",
             s_device_name,
             s_device_mac[0], s_device_mac[1], s_device_mac[2],
             s_device_mac[3], s_device_mac[4], s_device_mac[5]);

    /* 5. Initialize NimBLE Stack */
    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);

    ble_svc_gap_device_name_set(s_device_name);
    ble_hs_cfg.sync_cb = on_sync;

    nimble_port_freertos_init(nimble_host_task);

    /* 6. Start CPR Telemetry Task */
    xTaskCreate(cpr_telemetry_task, "cpr_telemetry", 4096, NULL, 5, NULL);
}
