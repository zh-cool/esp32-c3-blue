#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"

/* NimBLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_att.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* 业务模块 */
#include "envelope.h"
#include "ota.h"

static const char *TAG = "BLUE";

/* ======================== 常量 ======================== */

#define DEVICE_NAME "ESP32-C3-Blue"

#define GATT_SVC_UUID       0x00FF
#define GATT_CHR_UUID_TX    0xFF01
#define GATT_CHR_UUID_DATA  0xFF03

/* ======================== 全局状态 ======================== */

static uint16_t s_data_handle;  /* Custom Data 句柄 */

#define MAX_PEERS 3
static uint16_t s_peers[MAX_PEERS];
static int s_peer_count;

/* 前向声明 */
static void adv_start(void);

/* ======================== GATT 回调 ======================== */

static int desc_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const char *name = (const char *)arg;
    return os_mbuf_append(ctxt->om, name, strlen(name))
           ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

static int gatt_svc_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {

    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0) return 0;

        if (attr_handle == s_data_handle) {
            uint8_t buf[1024];
            if (len > sizeof(buf)) len = sizeof(buf);
            ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
            envelope_handle(conn_handle, buf, len);
            return 0;
        }

        /* TX 特征值 — 简单文本 */
        char tbuf[128];
        if (len > sizeof(tbuf) - 1) len = sizeof(tbuf) - 1;
        ble_hs_mbuf_to_flat(ctxt->om, tbuf, len, NULL);
        tbuf[len] = '\0';
        ESP_LOGI(TAG, "收到(TX): %s", tbuf);
        return 0;
    }

    case BLE_GATT_ACCESS_OP_READ_CHR:
        return 0;

    default:
        return 0;
    }
}

/* ======================== GATT 服务 ======================== */

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(GATT_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(GATT_CHR_UUID_TX),
              .access_cb = gatt_svc_access,
              .flags = BLE_GATT_CHR_F_WRITE,
              .descriptors = (struct ble_gatt_dsc_def[]) { {
                  .uuid = BLE_UUID16_DECLARE(0x2901),
                  .att_flags = BLE_ATT_F_READ,
                  .access_cb = desc_access,
                  .arg = (void *)"TX Data",
              }, { 0 } },
            }, {
                .uuid = BLE_UUID16_DECLARE(GATT_CHR_UUID_DATA),
                .access_cb = gatt_svc_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE
                       | BLE_GATT_CHR_F_NOTIFY,
                .descriptors = (struct ble_gatt_dsc_def[]) { {
                    .uuid = BLE_UUID16_DECLARE(0x2901),
                    .att_flags = BLE_ATT_F_READ,
                    .access_cb = desc_access,
                    .arg = (void *)"Custom Data",
                }, { 0 } },
            }, { 0 },
        },
    }, { 0 },
};

static const char *phy_str(uint8_t phy)
{
    switch (phy) {
    case BLE_GAP_LE_PHY_1M:    return "1M";
    case BLE_GAP_LE_PHY_2M:    return "2M";
    case BLE_GAP_LE_PHY_CODED: return "Coded";
    default:                    return "?";
    }
}

/* ======================== GAP 事件 ======================== */

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            uint16_t h = event->connect.conn_handle;
            struct ble_gap_conn_desc d;
            ESP_LOGI(TAG, "=== 已连接 (conn=%d) ===", h);
            if (ble_gap_conn_find(h, &d) == 0)
                ESP_LOGI(TAG, "  间隔: %u x 1.25ms, MTU: %u",
                         d.conn_itvl, ble_att_mtu(h));
            if (s_peer_count < MAX_PEERS) s_peers[s_peer_count++] = h;
            adv_start();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t h = event->disconnect.conn.conn_handle;
        ESP_LOGI(TAG, "已断开 (conn=%d, reason=%d)", h, event->disconnect.reason);
        for (int i = 0; i < s_peer_count; i++)
            if (s_peers[i] == h) { s_peers[i] = s_peers[--s_peer_count]; break; }
        ota_on_disconnect();
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU: conn=%d, mtu=%u", event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        ESP_LOGI(TAG, "PHY: conn=%d, TX=%s, RX=%s",
                 event->phy_updated.conn_handle,
                 phy_str(event->phy_updated.tx_phy),
                 phy_str(event->phy_updated.rx_phy));
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        return 0;

    default:
        return 0;
    }
}

/* ======================== 广播 ======================== */

static void adv_start(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;
    const char *name = ble_svc_gap_device_name();

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.uuids16 = (ble_uuid16_t[]) { BLE_UUID16_INIT(GATT_SVC_UUID) };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields fail (rc=%d)", rc); return; }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "广播中: %s", name);
    } else if (rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv_start fail (rc=%d)", rc);
    }
}

/* ======================== NimBLE 初始化 ======================== */

static void ble_host_sync(void)
{
    ble_gatts_find_chr(BLE_UUID16_DECLARE(GATT_SVC_UUID),
                       BLE_UUID16_DECLARE(GATT_CHR_UUID_DATA),
                       NULL, &s_data_handle);
    if (s_data_handle == 0) {
        ESP_LOGW(TAG, "未找到 DATA 句柄");
    } else {
        envelope_set_data_handle(s_data_handle);
    }
    adv_start();
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_app_init(void)
{
    ble_svc_gap_device_name_set(DEVICE_NAME);
    nimble_port_init();
    ble_hs_cfg.sync_cb = ble_host_sync;
    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);
    nimble_port_freertos_init(ble_host_task);
}

/* ======================== 主入口 ======================== */

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ota_init();

    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "  ESP32-C3 NimBLE + OTA");
    ESP_LOGI(TAG, "  FW: %s", esp_app_get_description()->version);
    ESP_LOGI(TAG, "  TX   (0xFF01): 文本写入");
    ESP_LOGI(TAG, "  Data (0xFF03): 读写+通知 Envelope");
    ESP_LOGI(TAG, "====================================");

    ble_app_init();
}
