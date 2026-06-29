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
static uint32_t s_last_log_pct;

/* ======================== 字符串编码 callback ======================== */

static bool msg_cb(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
    const char *s = *arg ? (const char *)(*arg) : "";
    if (*s == '\0') return true;
    return pb_encode_string(stream, (uint8_t *)s, strlen(s));
}

/* ======================== 构造并存入响应缓冲区 ======================== */

static void store_resp(uint32_t request_id, const led_control_OTAResponse *ota_r)
{
    led_control_EnvelopeResponse env_resp = led_control_EnvelopeResponse_init_zero;
    env_resp.request_id = request_id;
    env_resp.error = led_control_ErrorCode_OK;

    if (ota_r && ota_r->error != led_control_ErrorCode_OK) {
        env_resp.error = ota_r->error;
        env_resp.error_msg.funcs.encode = msg_cb;
    }

    if (ota_r) {
        env_resp.which_result = led_control_EnvelopeResponse_ota_result_tag;
        env_resp.result.ota_result = *ota_r;
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

/* 从 p 开始读 protobuf varint, 返回 (值, 新位置) */
static size_t read_varint(const uint8_t *d, size_t len, size_t p, uint32_t *val)
{
    *val = 0; int shift = 0;
    while (p < len) {
        *val |= (uint32_t)(d[p] & 0x7F) << shift;
        if (!(d[p++] & 0x80)) break;
        shift += 7;
    }
    return p;
}

/* 顺序扫描下一字节, 只要 != target 就前进 */
static size_t scan_to(const uint8_t *d, size_t len, size_t p, uint8_t target)
{
    while (p < len && d[p] != target) p++;
    return p;
}

/* ====== 从原始 protobuf 可靠解析 chunk bytes ======
 * 顺序扫描: 0x92(ota) → 0x5a(data_params) → 0x12(chunk)
 * 每次进入下一层后, 只在本层范围内查找, 避免固件数据中的假匹配
 */
static bool extract_chunk(const uint8_t *data, size_t len,
                          const uint8_t **out, size_t *out_len)
{
    size_t p = 0;
    uint32_t val;

    /* --- 先跳过 Envelope 头部 --- */
    /* protocol_version (field 1, varint): 08 [varint] */
    if (p >= len || data[p++] != 0x08) return false;
    p = read_varint(data, len, p, &val);
    /* request_id (field 2, varint): 10 [varint] */
    if (p >= len || data[p++] != 0x10) return false;
    p = read_varint(data, len, p, &val);
    /* 现在在 payload oneof, 找 ota 字段 (field 50, tag=0x92 vari...) */
    p = scan_to(data, len, p, 0x92);
    if (p >= len) return false;
    p++;                                 /* skip 0x92 */
    p = read_varint(data, len, p, &val); /* skip tag continuation + length */
    /* 现在 p 指向 OTARequest 内容的开头, 找 data_params (field 11, tag=0x5a) */
    p = scan_to(data, len, p, 0x5a);
    if (p >= len) return false;
    p++;                                 /* skip 0x5a */
    p = read_varint(data, len, p, &val); /* skip tag continuation + length */
    /* 现在 p 指向 OTADataParams 内容的开头 */
    /* 先跳过 offset (field 1, tag=0x08, 但可能含有 0x12 字节) */
    /* 唯一可靠的: 逐字节前进, 直到 0x12 且确定是 chunk tag 而非 offset 值内的字节 */
    /* offset 是 varint, 在 offset 的 varint 结束后至少有一个 tag 字节 */
    /* 先跳过 tag 0x08 */
    if (p < len && data[p] == 0x08) p++;
    else return false;
    p = read_varint(data, len, p, &val); /* skip offset value */
    /* 现在应该指向 chunk tag 0x12 */
    if (p >= len || data[p] != 0x12) return false;
    p++;                                 /* skip 0x12 */
    p = read_varint(data, len, p, &val); /* read chunk length */
    *out_len = val;
    if (p + *out_len > len) return false;
    *out = data + p;
    return true;
}

/* 延时重启任务 — 给手机留时间读取响应 */
static void delayed_restart(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "restarting now");
    esp_restart();
}

/* ======================== 处理 Envelope ======================== */

void ota_handle_envelope(const uint8_t *data, size_t len)
{
    pb_istream_t stream = pb_istream_from_buffer(data, len);
    led_control_Envelope env = led_control_Envelope_init_default;

    if (!pb_decode(&stream, led_control_Envelope_fields, &env)) {
        ESP_LOGW(TAG, "decode fail");
        return;
    }
    if (env.which_payload != led_control_Envelope_ota_tag) return;

    led_control_OTARequest *req = &env.payload.ota;
    uint32_t req_id = env.request_id;

    /* ================== CMD_DATA ================== */
    if (req->cmd == led_control_OTARequest_Cmd_CMD_DATA) {
        if (!s_active || req->which_params != led_control_OTARequest_data_params_tag) {
            ESP_LOGW(TAG, "DATA: not active");
            return;
        }

        const uint8_t *chunk_data;
        size_t chunk_len;
        if (!extract_chunk(data, len, &chunk_data, &chunk_len)) {
            ESP_LOGE(TAG, "DATA: no chunk found");
            return;
        }

        esp_err_t err = esp_ota_write(s_ota_handle, chunk_data, chunk_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write fail (err=%d)", err);
            esp_ota_abort(s_ota_handle);
            s_active = false;
            return;
        }
        s_ota_received += chunk_len;

        /* 每 10% 打印一次进度 */
        uint32_t new_pct = (s_ota_total > 0) ? (s_ota_received * 100 / s_ota_total) : 0;
        if (new_pct - (new_pct % 10) > s_last_log_pct) {
            s_last_log_pct = new_pct - (new_pct % 10);
            ESP_LOGI(TAG, "progress: %u%% (%u/%u)", s_last_log_pct, s_ota_received, s_ota_total);
        }

        led_control_OTAResponse r;
        memset(&r, 0, sizeof(r));
        r.error = led_control_ErrorCode_OK;
        r.received_bytes = s_ota_received;
        r.total_bytes = s_ota_total;
        r.percent = (s_ota_received * 100 / s_ota_total);
        store_resp(req_id, &r);
        return;
    }

    /* ================== 其他命令 ================== */
    led_control_OTAResponse resp;
    memset(&resp, 0, sizeof(resp));
    const char *err = NULL;

    switch (req->cmd) {

    case led_control_OTARequest_Cmd_CMD_START: {
        if (s_active) { err = "already started"; break; }
        if (req->which_params != led_control_OTARequest_start_params_tag) { err = "no params"; break; }
        s_ota_total = req->params.start_params.total_size;
        s_ota_received = 0;
        s_last_log_pct = 0;
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
        ESP_LOGI(TAG, "COMPLETE %u bytes, restart in 3s", s_ota_received);
        /* 独立任务延时重启, 不阻塞 NimBLE */
        xTaskCreate(delayed_restart, "ota_rb", 2048, NULL, 5, NULL);
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

    resp.received_bytes = s_ota_received;
    resp.total_bytes = s_ota_total;
    resp.percent = (s_ota_total > 0) ? (s_ota_received * 100 / s_ota_total) : 0;
    if (err) {
        resp.error = led_control_ErrorCode_ERR_OTA_WRITE_FLASH;
        resp.error_msg.arg = (void *)err;
    }
    store_resp(req_id, &resp);
}

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
    s_last_log_pct = 0;
}
