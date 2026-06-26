#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
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

/* RX 特征值句柄（用于发送通知） */
static uint16_t s_chr_rx_handle;

/* 已连接手机句柄列表 */
#define MAX_PEERS 3
static uint16_t s_peers[MAX_PEERS];
static int s_peer_count;

/* 通知计数器 + 定时器 */
static uint32_t s_notify_count;
static TimerHandle_t s_timer;

/* 前向声明 */
static void adv_start(void);

/* ======================== 通知发送 ======================== */

static void send_notification_to_peer(uint16_t conn_handle)
{
    struct os_mbuf *om;
    char buf[32];
    int len;

    if (s_chr_rx_handle == 0)
        return;

    len = snprintf(buf, sizeof(buf), "Hello %lu", (unsigned long)s_notify_count);
    om = ble_hs_mbuf_from_flat(buf, len);
    if (om == NULL) {
        ESP_LOGE(TAG, "分配 mbuf 失败");
        return;
    }

    int rc = ble_gatts_notify_custom(conn_handle, s_chr_rx_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "通知发送失败 (conn=%d, rc=%d)", conn_handle, rc);
        os_mbuf_free_chain(om);
    }
}

static void timer_callback(TimerHandle_t xTimer)
{
    if (s_peer_count == 0)
        return;

    s_notify_count++;

    for (int i = 0; i < s_peer_count; i++) {
        send_notification_to_peer(s_peers[i]);
    }
}

/* 描述符访问回调 — 返回特征值名称 */
static int desc_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const char *name = (const char *)arg;
    return os_mbuf_append(ctxt->om, name, strlen(name))
           ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

/* ======================== GATT 访问回调 ======================== */

static int gatt_svc_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {

    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        /* 手机写入数据到 TX 特征值 */
        char buf[16];
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > sizeof(buf) - 1)
            len = sizeof(buf) - 1;
        ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
        buf[len] = '\0';

        ESP_LOGI(TAG, "收到: %s", buf);

        /* Y -> 开始推送, N -> 停止推送 */
        if (len == 1) {
            if (buf[0] == 'Y' || buf[0] == 'y') {
                if (s_timer != NULL) {
                    s_notify_count = 0;
                    xTimerStart(s_timer, 0);
                    ESP_LOGI(TAG, "定时通知已启动 (每 5 秒)");
                }
            } else if (buf[0] == 'N' || buf[0] == 'n') {
                if (s_timer != NULL) {
                    xTimerStop(s_timer, 0);
                    ESP_LOGI(TAG, "定时通知已停止");
                }
            }
        }
        return 0;
    }

    case BLE_GATT_ACCESS_OP_READ_CHR: {
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
                .descriptors = (struct ble_gatt_dsc_def[]) { {
                    .uuid = BLE_UUID16_DECLARE(0x2901),
                    .att_flags = BLE_ATT_F_READ,
                    .access_cb = desc_access,
                    .arg = (void *)"TX Data",
                }, {
                    0,
                } },
            }, {
                /* RX: ESP32 -> 手机（可读 + 通知） */
                .uuid = BLE_UUID16_DECLARE(GATT_CHR_UUID_RX),
                .access_cb = gatt_svc_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .descriptors = (struct ble_gatt_dsc_def[]) { {
                    .uuid = BLE_UUID16_DECLARE(0x2901),
                    .att_flags = BLE_ATT_F_READ,
                    .access_cb = desc_access,
                    .arg = (void *)"RX Data",
                }, {
                    0,
                } },
            }, {
                0,
            },
        },
    }, {
        0,
    },
};

/* ======================== GAP 事件 ======================== */

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            uint16_t handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "手机已连接 (conn_handle=%d)", handle);
            if (s_peer_count < MAX_PEERS) {
                s_peers[s_peer_count++] = handle;
            }
            adv_start();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t handle = event->disconnect.conn.conn_handle;
        ESP_LOGI(TAG, "手机已断开 (conn=%d, reason=%d)", handle, event->disconnect.reason);
        for (int i = 0; i < s_peer_count; i++) {
            if (s_peers[i] == handle) {
                s_peers[i] = s_peers[--s_peer_count];
                break;
            }
        }
        /* 所有手机都断开时停止定时器 */
        if (s_peer_count == 0 && s_timer != NULL) {
            xTimerStop(s_timer, 0);
            ESP_LOGI(TAG, "无连接, 定时通知已停止");
        }
        return 0;
    }

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
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播包失败 (rc=%d)", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "广播中: %s", name);
    } else if (rc == BLE_HS_EALREADY) {
    } else {
        ESP_LOGE(TAG, "启动广播失败 (rc=%d)", rc);
    }
}

/* ======================== NimBLE 同步回调 ======================== */

static void ble_host_sync(void)
{
    int rc = ble_gatts_find_chr(
        BLE_UUID16_DECLARE(GATT_SVC_UUID),
        BLE_UUID16_DECLARE(GATT_CHR_UUID_RX),
        NULL, &s_chr_rx_handle);
    if (rc != 0) {
        ESP_LOGW(TAG, "未找到 RX 句柄 (rc=%d)", rc);
    }
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
    ble_svc_gap_device_name_set(DEVICE_NAME);
    nimble_port_init();

    ble_hs_cfg.sync_cb = ble_host_sync;

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "GATT 计数失败 (rc=%d)", rc); return; }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "GATT 注册失败 (rc=%d)", rc); return; }

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

    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "  ESP32-C3 NimBLE");
    ESP_LOGI(TAG, "  向 TX(0xFF01) 写 Y -> 每 5 秒推送");
    ESP_LOGI(TAG, "  向 TX(0xFF01) 写 N -> 停止推送");
    ESP_LOGI(TAG, "=================================");

    /* 创建定时器（5 秒周期）, 初始不启动 */
    s_timer = xTimerCreate("notify", pdMS_TO_TICKS(5000), pdTRUE,
                           NULL, timer_callback);

    ble_app_init();
}
