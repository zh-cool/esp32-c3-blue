#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Protobuf */
#include "pb_encode.h"
#include "pb_decode.h"
#include "led_control.pb.h"

#include "ota.h"
#include "envelope.h"

static const char *TAG = "OTA";

/* ======================== 状态 ======================== */

static bool s_active;
static esp_ota_handle_t s_ota_handle;
static const esp_partition_t *s_ota_partition;
static uint32_t s_ota_total;
static uint32_t s_ota_received;

/* chunk_cb 通过此静态变量获取请求 ID（避免 nanopb 清零 oneof 子消息时覆盖 callback arg） */
static uint32_t s_chunk_req_id;

/* ======================== 字符串编码 callback ======================== */

/* CALLBACK 编码 error_msg — nanopb 自动写 tag, callback 只写值 */
static bool msg_cb(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
    const char *s = *arg ? (const char *)(*arg) : "";
    if (*s == '\0') return true;  /* 空消息 → 跳过, 避免写裸 00 */
    return pb_encode_string(stream, (uint8_t *)s, strlen(s));
}

/* ======================== 构造并存入响应缓冲区 ======================== */

static void store_resp(uint32_t request_id, const led_control_OTAResponse *ota_r)
{
    led_control_EnvelopeResponse env_resp = led_control_EnvelopeResponse_init_zero;
    env_resp.request_id = request_id;
    env_resp.error = led_control_ErrorCode_OK;
    /* EnvelopeResponse.error_msg — 报错时才传 */
    if (ota_r && ota_r->error != led_control_ErrorCode_OK) {
        env_resp.error = ota_r->error;
        env_resp.error_msg.funcs.encode = msg_cb;
    }

    if (ota_r) {
        env_resp.which_result = led_control_EnvelopeResponse_ota_result_tag;
        env_resp.result.ota_result = *ota_r;
        /* 仅当 error_msg 非空时才设 CALLBACK, 空消息跳过 */
        if (ota_r->error_msg.arg && *(const char *)(ota_r->error_msg.arg)) {
            env_resp.result.ota_result.error_msg.funcs.encode = msg_cb;
        }
    }

    led_control_Envelope env = led_control_Envelope_init_zero;
    env.protocol_version = 2;
    env.request_id = request_id;
    env.which_payload = led_control_Envelope_response_tag;
    env.payload.response = env_resp;

    pb_ostream_t s = pb_ostream_from_buffer(envelope_resp_buf, sizeof(envelope_resp_buf));
    if (pb_encode(&s, led_control_Envelope_fields, &env))
        envelope_resp_len = s.bytes_written;
    else
        ESP_LOGE(TAG, "encode fail: %s", PB_GET_ERROR(&s));
}

/* ======================== 数据块解码 callback ======================== */

static bool chunk_cb(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
    (void)arg;
    uint32_t req_id = s_chunk_req_id;
    bool first = true;

    while (stream->bytes_left) {
        uint8_t buf[512];
        size_t to_read = stream->bytes_left;
        if (to_read > sizeof(buf)) to_read = sizeof(buf);
        if (!pb_read(stream, buf, to_read)) return false;

        esp_err_t err = esp_ota_write(s_ota_handle, buf, to_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write fail (err=%d)", err);
            return false;
        }
        s_ota_received += to_read;

        if (first || stream->bytes_left == 0) {
            led_control_OTAResponse r = led_control_OTAResponse_init_zero;
            r.error = led_control_ErrorCode_OK;
            r.received_bytes = s_ota_received;
            r.total_bytes = s_ota_total;
            r.percent = (s_ota_received * 100 / s_ota_total);
            r.error_msg.funcs.encode = msg_cb;
            store_resp(req_id, &r);
            first = false;
        }
    }
    return true;
}

/* ======================== 处理 Envelope ======================== */

/* 从原始 protobuf 数据中提取 field 2 (request_id) 的 varint 值 */
static uint32_t peek_req_id(const uint8_t *data, size_t len)
{
    /* tag for field 2 varint = 0x10 */
    for (size_t i = 0; i + 1 < len; i++) {
        if (data[i] == 0x10) {
            uint32_t v = 0; int s = 0;
            for (size_t j = i + 1; j < len; j++, s += 7) {
                v |= (uint32_t)(data[j] & 0x7F) << s;
                if (!(data[j] & 0x80)) return v;
            }
        }
    }
    return 0;
}

void ota_handle_envelope(const uint8_t *data, size_t len)
{
    pb_istream_t stream = pb_istream_from_buffer(data, len);
    led_control_Envelope env = led_control_Envelope_init_default;

    /* 预先提取 request_id 供 chunk_cb 使用 */
    s_chunk_req_id = peek_req_id(data, len);

    /* 预先设 chunk callback — 只有 CMD_DATA 含 chunk 字段才会触发 */
    env.payload.ota.params.data_params.chunk.funcs.decode = chunk_cb;

    if (!pb_decode(&stream, led_control_Envelope_fields, &env)) {
        ESP_LOGW(TAG, "decode fail");
        return;
    }
    if (env.which_payload != led_control_Envelope_ota_tag) return;

    led_control_OTARequest *req = &env.payload.ota;
    req_id = env.request_id;

    /* CMD_DATA 已在解码过程中由 chunk_cb 处理并调用 store_resp */
    if (req->cmd == led_control_OTARequest_Cmd_CMD_DATA) {
        if (s_active && req->which_params == led_control_OTARequest_data_params_tag)
            ESP_LOGI(TAG, "DATA: offset=%u OK", req->params.data_params.offset);
        else
            ESP_LOGW(TAG, "DATA: no data or not active");
        return;
    }

    /* START / COMPLETE / ABORT */
    led_control_OTAResponse resp = led_control_OTAResponse_init_zero;
    const char *err = NULL;

    switch (req->cmd) {

    case led_control_OTARequest_Cmd_CMD_START: {
        if (s_active) { err = "already started"; break; }
        if (req->which_params != led_control_OTARequest_start_params_tag) { err = "no params"; break; }
        s_ota_total = req->params.start_params.total_size;
        s_ota_received = 0;
        ESP_LOGI(TAG, "START total=%u", s_ota_total);
        s_ota_partition = esp_ota_get_next_update_partition(NULL);
        if (!s_ota_partition) { err = "no partition"; break; }
        if (esp_ota_begin(s_ota_partition, s_ota_total, &s_ota_handle) != ESP_OK) { err = "begin fail"; break; }
        s_active = true;
        resp.received_bytes = 0;
        resp.total_bytes = s_ota_total;
        resp.percent = 0;
        break;
    }

    case led_control_OTARequest_Cmd_CMD_DATA: {
        if (!s_active) { err = "not started"; break; }
        if (req->which_params != led_control_OTARequest_data_params_tag) { err = "no data"; break; }

        ESP_LOGI(TAG, "DATA: offset=%u (first decode already consumed chunk)",
                 req->params.data_params.offset);

        /* chunk 已被首次解码消耗(无 callback 丢弃), 用原始数据二次解码 */
        s_chunk_req_id = req_id;
        pb_istream_t s2 = pb_istream_from_buffer(data, len);
        led_control_Envelope e2 = led_control_Envelope_init_zero;
        e2.payload.ota.params.data_params.chunk.funcs.decode = chunk_cb;
        if (!pb_decode(&s2, led_control_Envelope_fields, &e2))
            ESP_LOGE(TAG, "chunk decode FAIL");
        else
            ESP_LOGI(TAG, "chunk decode OK");
        return;
    }

    case led_control_OTARequest_Cmd_CMD_COMPLETE: {
        if (!s_active) { err = "not started"; break; }
        if (esp_ota_end(s_ota_handle) != ESP_OK) { err = "ota_end fail"; s_active = false; break; }
        s_active = false;
        if (esp_ota_set_boot_partition(s_ota_partition) != ESP_OK) { err = "set_boot fail"; break; }
        resp.received_bytes = s_ota_received;
        resp.total_bytes = s_ota_total;
        resp.percent = 100; resp.done = true;
        resp.error_msg.arg = (void *)"OK, restarting...";
        store_resp(req_id, &resp);
        ESP_LOGI(TAG, "COMPLETE %u bytes", s_ota_received);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return;
    }

    case led_control_OTARequest_Cmd_CMD_ABORT:
        if (s_active) { esp_ota_abort(s_ota_handle); s_active = false; }
        ESP_LOGI(TAG, "ABORT");
        err = "aborted";
        break;

    default:
        err = "unknown cmd";
        break;
    }

    /* 填充进度并发送响应 */
    resp.received_bytes = s_ota_received;
    resp.total_bytes = s_ota_total;
    resp.percent = (s_ota_total > 0) ? (s_ota_received * 100 / s_ota_total) : 0;
    if (err) {
        resp.error = led_control_ErrorCode_ERR_OTA_WRITE_FLASH;
        resp.error_msg.arg = (void *)err;
    }
    store_resp(req_id, &resp);
}

/* ======================== 断开 ======================== */

void ota_on_disconnect(void)
{
    if (s_active) {
        esp_ota_abort(s_ota_handle);
        s_active = false;
        ESP_LOGI(TAG, "abort (disconnect)");
    }
}

void ota_init(void)
{
    s_active = false;
    s_ota_total = 0;
    s_ota_received = 0;
}
