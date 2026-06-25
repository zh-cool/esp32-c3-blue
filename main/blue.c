#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

/* NimBLE 协议栈 */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "BLUE";

/* 设备名称 */
#define DEVICE_NAME "ESP32-C3-Blue"

/* 自定义服务 UUID（16-bit） */
#define GATT_SVC_UUID       0x00FF
#define GATT_CHR_UUID_TX    0xFF01   /* 手机 -> ESP32（写）*/
#define GATT_CHR_UUID_RX    0xFF02   /* ESP32 -> 手机（读/通知）*/

/* 接收缓冲区 */
static char s_rx_buf[256];

/* RX 特征值句柄（用于发送通知） */
static uint16_t s_chr_rx_handle;

/* 前向声明 */
static void adv_start(void);

/* ======================== GATT 访问回调 ======================== */

static int gatt_svc_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {

    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        /* 手机写入数据到 TX 特征值 */
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > sizeof(s_rx_buf) - 1)
            len = sizeof(s_rx_buf) - 1;
        ble_hs_mbuf_to_flat(ctxt->om, s_rx_buf, len, NULL);
        s_rx_buf[len] = '\0';

        ESP_LOGI(TAG, "收到 (%d 字节): %s", len, s_rx_buf);
        return 0;
    }

    case BLE_GATT_ACCESS_OP_READ_CHR: {
        /* 手机读取 RX 特征值 */
        static const char resp[] = "Hello from ESP32-C3!";
        int rc = os_mbuf_append(ctxt->om, resp, strlen(resp));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    default:
        break;
    }
    return 0;
}

/* ======================== GATT 服务定义 ======================== */

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(GATT_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* TX: 手机 -> ESP32（可写） */
                .uuid = BLE_UUID16_DECLARE(GATT_CHR_UUID_TX),
                .access_cb = gatt_svc_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            }, {
                /* RX: ESP32 -> 手机（可读 + 通知） */
                .uuid = BLE_UUID16_DECLARE(GATT_CHR_UUID_RX),
                .access_cb = gatt_svc_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            }, {
                0, /* 终止 */
            },
        },
    }, {
        0, /* 终止 */
    },
};

/* ======================== GAP 事件 ======================== */

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "手机已连接 (conn_handle=%d)", event->connect.conn_handle);
            ESP_LOGI(TAG, "继续广播, 等待其他手机连接...");
            adv_start();
        } else {
            ESP_LOGE(TAG, "连接失败, 重新广播");
            adv_start();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "手机已断开, 重新广播");
        adv_start();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "广播结束, 重新开始");
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

    /* ---- 广播包 ---- */
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    /* 广播服务 UUID */
    fields.uuids16 = (ble_uuid16_t[]) { BLE_UUID16_INIT(GATT_SVC_UUID) };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播包失败 (rc=%d)", rc);
        return;
    }

    /* ---- 启动广播 ---- */
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    /* 先停止已有广播（防止 BLE_HS_EALREADY） */
    ble_gap_adv_stop();

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "广播中: %s", name);
    } else {
        ESP_LOGE(TAG, "启动广播失败 (rc=%d)", rc);
    }
}

/* ======================== NimBLE 主机同步回调 ========================
 * 在 NimBLE 主机与蓝牙控制器同步完成后调用。
 * 此时才能安全调用 GATT/GAP API。
 */

static void ble_host_sync(void)
{
    int rc;

    ESP_LOGI(TAG, "NimBLE 主机已同步");

    /* 查找 RX 特征值句柄（用于发送通知） */
    rc = ble_gatts_find_chr(
        BLE_UUID16_DECLARE(GATT_SVC_UUID),
        BLE_UUID16_DECLARE(GATT_CHR_UUID_RX),
        NULL, &s_chr_rx_handle);
    if (rc != 0) {
        ESP_LOGW(TAG, "未找到 RX 特征值句柄 (rc=%d)", rc);
    } else {
        ESP_LOGI(TAG, "RX 特征值句柄: %d", s_chr_rx_handle);
    }

    /* 开始广播 */
    adv_start();
}

/* ======================== NimBLE 主机任务 ======================== */

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ======================== 初始化 ======================== */

static void ble_app_init(void)
{
    int rc;

    /* 初始化 NimBLE 主机 */
    nimble_port_init();

    /* 设置 GAP 设备名称 */
    ble_svc_gap_device_name_set(DEVICE_NAME);

    /* 注册主机同步回调 */
    ble_hs_cfg.sync_cb = ble_host_sync;

    /* 注册 GATT 服务 */
    rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT 计数失败 (rc=%d)", rc);
        return;
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT 注册失败 (rc=%d)", rc);
        return;
    }

    /* 启动 NimBLE 主机任务（之后 ble_host_sync 会被异步调用） */
    nimble_port_freertos_init(ble_host_task);
}

/* ======================== 主入口 ======================== */

void app_main(void)
{
    /* 初始化 NVS（NimBLE 需要用于存储 bonding 信息） */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "  ESP32-C3 NimBLE 示例启动");
    ESP_LOGI(TAG, "  设备名: %s", DEVICE_NAME);
    ESP_LOGI(TAG, "  服务  : 0x00FF");
    ESP_LOGI(TAG, "  TX    : 0xFF01 (手机写)");
    ESP_LOGI(TAG, "  RX    : 0xFF02 (手机读/通知)");
    ESP_LOGI(TAG, "=================================");

    ble_app_init();
}
