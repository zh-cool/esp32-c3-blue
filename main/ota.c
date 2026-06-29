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

/* ======================== 构造并存入响应缓冲区 ======================== */

static void store_resp(uint32_t request_id, const led_control_OTAResponse *ota_r)
{
    led_control_EnvelopeResponse env_resp = led_control_EnvelopeResponse_init_zero;
    env_resp.request_id = request_id;
    env_resp.error = led_control_ErrorCode_OK;

    if (ota_r && ota_r->error != led_control_ErrorCode_OK) {
        env_resp.error = ota_r->error;
        strncpy(env_resp.error_msg, ota_r->error_msg, sizeof(env_resp.error_msg) - 1);
    }

    if (ota_r) {
        env_resp.which_result = led_control_EnvelopeResponse_ota_result_tag;
        env_resp.result.ota_result = *ota_r;
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

/* 延时重启任务 */
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

        uint8_t *chunk_data = req->params.data_params.chunk.bytes;
        size_t chunk_len = req->params.data_params.chunk.size;

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
        strncpy(resp.error_msg, "OK, restarting...", sizeof(resp.error_msg) - 1);
        store_resp(req_id, &resp);
        ESP_LOGI(TAG, "COMPLETE %u bytes, restart in 3s", s_ota_received);
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
        strncpy(resp.error_msg, err ? err : "", sizeof(resp.error_msg) - 1);
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
