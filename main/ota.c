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

/* ======================== 状态 ======================== */

static bool s_ota_active;
static esp_ota_handle_t s_ota_handle;
static const esp_partition_t *s_ota_partition;
static uint32_t s_ota_total;
static uint32_t s_ota_received;
static uint16_t s_data_handle;

/* ======================== 通知发送 ======================== */

static void send_notify(uint16_t conn_handle, const uint8_t *data, size_t len)
{
    if (s_data_handle == 0) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return;
    int rc = ble_gatts_notify_custom(conn_handle, s_data_handle, om);
    if (rc) os_mbuf_free_chain(om);
}

/* ======================== 字符串编码 callback ======================== */

static bool str_cb(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
    const char *s = (const char *)(*arg);
    if (!s) s = "";
    return pb_encode_string(stream, (uint8_t *)s, strlen(s));
}

/* ======================== 发送 EnvelopeResponse ======================== */

static void send_resp(uint16_t conn_handle, const led_control_OTAResponse *ota_resp,
                      const char *err_msg)
{
    led_control_EnvelopeResponse env_resp = led_control_EnvelopeResponse_init_default;
    env_resp.error = ota_resp->error;
    env_resp.which_result = led_control_EnvelopeResponse_ota_result_tag;
    env_resp.result.ota_result = *ota_resp;
    env_resp.error_msg.arg = (void *)(err_msg ? err_msg : "");
    env_resp.error_msg.funcs.encode = str_cb;

    uint8_t buf[256];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));

    led_control_Envelope env = led_control_Envelope_init_default;
    env.protocol_version = 2;
    env.which_payload = led_control_Envelope_response_tag;
    env.payload.response = env_resp;

    if (pb_encode(&stream, led_control_Envelope_fields, &env))
        send_notify(conn_handle, buf, stream.bytes_written);
    else
        ESP_LOGE(TAG, "编码失败: %s", PB_GET_ERROR(&stream));
}

/* ======================== 数据块解码 callback ========================
 * nanopb 解码到 chunk bytes 时调用, 直接写入 OTA 分区 */

static bool chunk_decode_cb(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
    uint16_t conn_handle = *(uint16_t *)arg;

    while (stream->bytes_left) {
        uint8_t buf[512];
        size_t to_read = stream->bytes_left;
        if (to_read > sizeof(buf)) to_read = sizeof(buf);

        if (!pb_read(stream, buf, to_read)) return false;

        esp_err_t err = esp_ota_write(s_ota_handle, buf, to_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write 失败 (err=%d)", err);
            return false;
        }
        s_ota_received += to_read;
        ESP_LOGI(TAG, "DATA: +%zu, total=%u, %u%%",
                 to_read, s_ota_received,
                 (s_ota_received * 100 / s_ota_total));
    }

    /* 发送进度响应 */
    led_control_OTAResponse resp = led_control_OTAResponse_init_default;
    resp.error = led_control_ErrorCode_OK;
    resp.received_bytes = s_ota_received;
    resp.total_bytes = s_ota_total;
    resp.percent = (s_ota_received * 100 / s_ota_total);
    send_resp(conn_handle, &resp, NULL);

    return true;
}

/* ======================== 处理 Envelope ======================== */

void ota_handle_envelope(uint16_t conn_handle, const uint8_t *data, size_t len)
{
    pb_istream_t stream = pb_istream_from_buffer(data, len);
    led_control_Envelope env = led_control_Envelope_init_default;

    if (!pb_decode(&stream, led_control_Envelope_fields, &env)) {
        ESP_LOGW(TAG, "Envelope 解码失败: %s", PB_GET_ERROR(&stream));
        return;
    }

    if (env.which_payload != led_control_Envelope_ota_tag) return;

    led_control_OTARequest *req = &env.payload.ota;

    led_control_OTAResponse resp = led_control_OTAResponse_init_default;
    const char *err_msg = NULL;
    resp.error = led_control_ErrorCode_OK;
    resp.received_bytes = s_ota_received;
    resp.total_bytes = s_ota_total;
    resp.percent = (s_ota_total > 0) ? (s_ota_received * 100 / s_ota_total) : 0;
    resp.done = false;

    switch (req->cmd) {

    case led_control_OTARequest_Cmd_CMD_START: {
        if (s_ota_active) {
            resp.error = led_control_ErrorCode_ERR_OTA_ALREADY_STARTED;
            err_msg = "OTA: already started"; break;
        }
        if (req->which_params != led_control_OTARequest_start_params_tag) {
            resp.error = led_control_ErrorCode_ERR_INVALID_PARAM;
            err_msg = "missing start_params"; break;
        }

        s_ota_total = req->params.start_params.total_size;
        s_ota_received = 0;
        s_ota_partition = esp_ota_get_next_update_partition(NULL);
        if (!s_ota_partition) {
            resp.error = led_control_ErrorCode_ERR_OTA_WRITE_FLASH;
            err_msg = "no OTA partition"; break;
        }

        esp_err_t err = esp_ota_begin(s_ota_partition, s_ota_total, &s_ota_handle);
        if (err != ESP_OK) {
            resp.error = led_control_ErrorCode_ERR_OTA_WRITE_FLASH;
            err_msg = "ota_begin failed"; break;
        }
        s_ota_active = true;
        resp.percent = 0; resp.received_bytes = 0;
        ESP_LOGI(TAG, "START: total=%u, partition=%s", s_ota_total, s_ota_partition->label);
        break;
    }

    case led_control_OTARequest_Cmd_CMD_DATA: {
        if (!s_ota_active) {
            resp.error = led_control_ErrorCode_ERR_OTA_NOT_STARTED;
            err_msg = "OTA: not started"; break;
        }
        if (req->which_params != led_control_OTARequest_data_params_tag) {
            resp.error = led_control_ErrorCode_ERR_INVALID_PARAM;
            err_msg = "missing data_params"; break;
        }

        /* 重新解码 Envelope，这一次捕获 chunk 写入 flash */
        pb_istream_t stream2 = pb_istream_from_buffer(data, len);
        led_control_Envelope env2 = led_control_Envelope_init_default;

        /* 为 chunk 字段设置 decode callback */
        env2.payload.ota.params.data_params.chunk.funcs.decode = chunk_decode_cb;
        env2.payload.ota.params.data_params.chunk.arg = &conn_handle;

        if (!pb_decode(&stream2, led_control_Envelope_fields, &env2)) {
            ESP_LOGE(TAG, "DATA decode 失败: %s", PB_GET_ERROR(&stream2));
            esp_ota_abort(s_ota_handle);
            s_ota_active = false;
        }
        return; /* chunk_decode_cb 已经发送了进度响应 */
    }

    case led_control_OTARequest_Cmd_CMD_COMPLETE: {
        if (!s_ota_active) {
            resp.error = led_control_ErrorCode_ERR_OTA_NOT_STARTED;
            err_msg = "OTA: not started"; break;
        }
        esp_err_t err = esp_ota_end(s_ota_handle);
        s_ota_active = false;
        if (err != ESP_OK) { resp.error = led_control_ErrorCode_ERR_OTA_WRITE_FLASH; err_msg = "ota_end failed"; break; }

        err = esp_ota_set_boot_partition(s_ota_partition);
        if (err != ESP_OK) { resp.error = led_control_ErrorCode_ERR_OTA_WRITE_FLASH; err_msg = "set_boot failed"; break; }

        resp.received_bytes = s_ota_received;
        resp.percent = 100; resp.done = true;
        err_msg = "OTA complete, restarting...";
        ESP_LOGI(TAG, "COMPLETE: %u bytes", s_ota_received);

        send_resp(conn_handle, &resp, err_msg);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return;
    }

    case led_control_OTARequest_Cmd_CMD_ABORT:
        if (s_ota_active) { esp_ota_abort(s_ota_handle); s_ota_active = false; }
        ESP_LOGI(TAG, "ABORTED");
        err_msg = "OTA aborted";
        break;

    default:
        resp.error = led_control_ErrorCode_ERR_UNKNOWN;
        err_msg = "unknown cmd";
        break;
    }

    send_resp(conn_handle, &resp, err_msg);
}

/* ======================== 断开 ======================== */

void ota_on_disconnect(void)
{
    if (s_ota_active) {
        esp_ota_abort(s_ota_handle);
        s_ota_active = false;
        ESP_LOGI(TAG, "OTA 终止 (断开)");
    }
}

/* ======================== 初始化 ======================== */

void ota_init(void)
{
    s_ota_active = false;
    s_ota_total = 0;
    s_ota_received = 0;
}

void ota_set_data_handle(uint16_t handle)
{
    s_data_handle = handle;
}
