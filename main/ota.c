#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* NimBLE */
#include "host/ble_hs.h"
#include "host/ble_gatt.h"

/* Protobuf */
#include "pb_encode.h"
#include "pb_decode.h"
#include "led_control.pb.h"

#include "ota.h"

static const char *TAG = "OTA";

/* ======================== OTA 状态 ======================== */

static bool s_in_progress;
static esp_ota_handle_t s_ota_handle;
static const esp_partition_t *s_ota_partition;
static uint32_t s_ota_total_size;
static uint32_t s_ota_received;

/* Custom Data 特征值句柄 — 用于发送通知响应 */
static uint16_t s_data_handle;

/* ======================== 通知发送 ======================== */

static void send_notify(uint16_t conn_handle, const uint8_t *data, size_t len)
{
    if (s_data_handle == 0) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) { ESP_LOGE(TAG, "mbuf 分配失败"); return; }
    int rc = ble_gatts_notify_custom(conn_handle, s_data_handle, om);
    if (rc) os_mbuf_free_chain(om);
}

/* ======================== 字符串编码 callback ======================== */

static bool str_callback(pb_ostream_t *stream, const pb_field_t *field,
                         void * const *arg)
{
    const char *str = (const char *)(*arg);
    if (!str) str = "";
    return pb_encode_string(stream, (uint8_t *)str, strlen(str));
}

/* ======================== 发送 EnvelopeResponse ======================== */

static void send_response(uint16_t conn_handle, led_control_OTAResponse *ota_resp,
                          const char *err_msg, bool will_restart)
{
    led_control_EnvelopeResponse env_resp = led_control_EnvelopeResponse_init_default;
    env_resp.error = ota_resp->error;
    env_resp.which_result = led_control_EnvelopeResponse_ota_result_tag;
    env_resp.result.ota_result = *ota_resp;
    env_resp.error_msg.arg = (void *)(err_msg ? err_msg : "");
    env_resp.error_msg.funcs.encode = str_callback;

    uint8_t buf[256];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));

    led_control_Envelope env = led_control_Envelope_init_default;
    env.protocol_version = 2;
    env.which_payload = led_control_Envelope_response_tag;
    env.payload.response = env_resp;

    if (pb_encode(&stream, led_control_Envelope_fields, &env)) {
        send_notify(conn_handle, buf, stream.bytes_written);
        if (!will_restart)
            ESP_LOGI(TAG, "响应: error=%d, %u%%", ota_resp->error, ota_resp->percent);
    } else {
        ESP_LOGE(TAG, "编码失败: %s", PB_GET_ERROR(&stream));
    }
}

/* ======================== OTA 命令处理 ======================== */

void ota_handle_cmd(uint16_t conn_handle, const led_control_OTARequest *req)
{
    led_control_OTAResponse resp = led_control_OTAResponse_init_default;
    const char *err_msg = NULL;

    resp.error = led_control_ErrorCode_OK;
    resp.done = false;
    resp.received_bytes = s_ota_received;
    resp.total_bytes = s_ota_total_size;
    resp.percent = (s_ota_total_size > 0)
        ? (s_ota_received * 100 / s_ota_total_size) : 0;

    switch (req->cmd) {

    case led_control_OTARequest_Cmd_CMD_START: {
        if (s_in_progress) {
            resp.error = led_control_ErrorCode_ERR_OTA_ALREADY_STARTED;
            err_msg = "OTA already in progress";
            resp.received_bytes = 0; resp.percent = 0;
            break;
        }
        if (req->which_params != led_control_OTARequest_start_params_tag) {
            resp.error = led_control_ErrorCode_ERR_INVALID_PARAM;
            err_msg = "missing start_params";
            break;
        }
        s_ota_total_size = req->params.start_params.total_size;
        s_ota_received = 0;

        s_ota_partition = esp_ota_get_next_update_partition(NULL);
        if (!s_ota_partition) {
            resp.error = led_control_ErrorCode_ERR_OTA_WRITE_FLASH;
            err_msg = "no OTA partition"; break;
        }
        esp_err_t err = esp_ota_begin(s_ota_partition, s_ota_total_size, &s_ota_handle);
        if (err != ESP_OK) {
            resp.error = led_control_ErrorCode_ERR_OTA_WRITE_FLASH;
            err_msg = "ota_begin failed"; break;
        }
        s_in_progress = true;
        resp.received_bytes = 0; resp.percent = 0;
        ESP_LOGI(TAG, "START: total=%u, partition=%s",
                 s_ota_total_size, s_ota_partition->label);
        break;
    }

    case led_control_OTARequest_Cmd_CMD_COMPLETE: {
        if (!s_in_progress) {
            resp.error = led_control_ErrorCode_ERR_OTA_NOT_STARTED;
            err_msg = "OTA not started"; break;
        }
        esp_err_t err = esp_ota_end(s_ota_handle);
        s_in_progress = false;
        if (err != ESP_OK) { resp.error = led_control_ErrorCode_ERR_OTA_WRITE_FLASH; err_msg = "ota_end failed"; break; }

        err = esp_ota_set_boot_partition(s_ota_partition);
        if (err != ESP_OK) { resp.error = led_control_ErrorCode_ERR_OTA_WRITE_FLASH; err_msg = "set_boot failed"; break; }

        resp.received_bytes = s_ota_received;
        resp.percent = 100; resp.done = true;
        err_msg = "OTA complete, restarting...";
        ESP_LOGI(TAG, "COMPLETE: %u bytes, rebooting", s_ota_received);

        send_response(conn_handle, &resp, err_msg, true);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return;
    }

    case led_control_OTARequest_Cmd_CMD_ABORT:
        if (s_in_progress) { esp_ota_abort(s_ota_handle); s_in_progress = false; }
        ESP_LOGI(TAG, "ABORTED");
        err_msg = "OTA aborted";
        break;

    default:
        resp.error = led_control_ErrorCode_ERR_UNKNOWN;
        err_msg = "unknown cmd";
        break;
    }

    send_response(conn_handle, &resp, err_msg, false);
}

/* ======================== 裸数据写入（OTA 进行中）================== */

void ota_handle_data(uint16_t conn_handle, const uint8_t *data, size_t len)
{
    if (!s_in_progress) return;

    esp_err_t err = esp_ota_write(s_ota_handle, data, len);
    if (err == ESP_OK) {
        s_ota_received += len;
        ESP_LOGI(TAG, "DATA: +%u, total=%u, %u%%",
                 len, s_ota_received,
                 (s_ota_received * 100 / s_ota_total_size));
    } else {
        ESP_LOGE(TAG, "ota_write 失败 (err=%d)", err);
    }
}

/* ======================== 中断处理 ======================== */

void ota_on_disconnect(void)
{
    if (s_in_progress) {
        esp_ota_abort(s_ota_handle);
        s_in_progress = false;
        ESP_LOGI(TAG, "OTA 终止 (断开)");
    }
}

/* ======================== 初始化 ======================== */

void ota_init(void)
{
    s_in_progress = false;
    s_ota_total_size = 0;
    s_ota_received = 0;
}

void ota_set_data_handle(uint16_t handle)
{
    s_data_handle = handle;
}

bool ota_is_in_progress(void)
{
    return s_in_progress;
}
