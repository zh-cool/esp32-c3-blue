#include <string.h>
#include "esp_log.h"

/* Protobuf */
#include "pb_encode.h"
#include "pb_decode.h"
#include "led_control.pb.h"

#include "envelope.h"
#include "ota.h"

static const char *TAG = "ENV";

/* 响应缓冲区 */
uint8_t envelope_resp_buf[1024];
uint16_t envelope_resp_len;

/* ======================== 字符串编码 callback ======================== */

static bool str_cb(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
    const char *s = *arg ? (const char *)(*arg) : "";
    if (*s == '\0') return true;  /* 空 → 跳过, 避免写裸 00 */
    return pb_encode_string(stream, (uint8_t *)s, strlen(s));
}

/* ======================== 存入响应缓冲区 ======================== */

static void store_response(uint32_t request_id, led_control_ErrorCode error,
                           const char *msg, led_control_EnvelopeResponse *resp)
{
    if (!resp) {
        /* 纯错误响应 */
        led_control_EnvelopeResponse r = led_control_EnvelopeResponse_init_default;
        r.request_id = request_id;
        r.error = error;
        r.error_msg.arg = (void *)(msg ? msg : "");
        r.error_msg.funcs.encode = str_cb;

        led_control_Envelope env = led_control_Envelope_init_default;
        env.protocol_version = 2;
        env.request_id = request_id;
        env.which_payload = led_control_Envelope_response_tag;
        env.payload.response = r;

        pb_ostream_t s = pb_ostream_from_buffer(envelope_resp_buf, sizeof(envelope_resp_buf));
        if (pb_encode(&s, led_control_Envelope_fields, &env))
            envelope_resp_len = s.bytes_written;
        return;
    }

    /* 自定义响应 — 确保所有 CALLBACK 字段都有编码器 */
    if (resp->error_msg.funcs.encode == NULL) {
        /* 为深拷贝的 error_msg 设置空字符串编码 */
        ((led_control_EnvelopeResponse *)resp)->error_msg.funcs.encode = str_cb;
        ((led_control_EnvelopeResponse *)resp)->error_msg.arg = (void *)"";
    }

    led_control_Envelope env = led_control_Envelope_init_default;
    env.protocol_version = 2;
    env.request_id = request_id;
    env.which_payload = led_control_Envelope_response_tag;
    env.payload.response = *resp;

    pb_ostream_t s = pb_ostream_from_buffer(envelope_resp_buf, sizeof(envelope_resp_buf));
    if (pb_encode(&s, led_control_Envelope_fields, &env))
        envelope_resp_len = s.bytes_written;
}

/* ======================== 各消息类型处理 ======================== */

static void handle_get_device_info(uint32_t req_id)
{
    /* TODO: 填充真实设备信息 */
    led_control_EnvelopeResponse r = led_control_EnvelopeResponse_init_default;
    r.request_id = req_id;
    r.error = led_control_ErrorCode_OK;
    r.which_result = led_control_EnvelopeResponse_device_info_result_tag;
    r.result.device_info_result.info.fw_version.arg = (void *)"1.0.0";
    r.result.device_info_result.info.fw_version.funcs.encode = str_cb;
    store_response(req_id, led_control_ErrorCode_OK, NULL, &r);
}

/* ======================== 主入口 ======================== */

void envelope_handle(uint16_t conn_handle, const uint8_t *data, size_t len)
{
    (void)conn_handle;

    pb_istream_t stream = pb_istream_from_buffer(data, len);
    led_control_Envelope env = led_control_Envelope_init_default;

    if (!pb_decode(&stream, led_control_Envelope_fields, &env)) {
        ESP_LOGW(TAG, "decode fail: %s", PB_GET_ERROR(&stream));
        return;
    }

    ESP_LOGI(TAG, "type=%d, req=%u", env.which_payload, env.request_id);

    switch (env.which_payload) {

    case led_control_Envelope_ota_tag:
        /* OTA 模块自己写入响应缓冲区 */
        ota_handle_envelope(data, len);
        return;

    case led_control_Envelope_get_device_info_tag:
        handle_get_device_info(env.request_id);
        return;

    /* 其他 — 暂不支持 */
    case led_control_Envelope_set_light_config_tag:
    case led_control_Envelope_set_channel_order_tag:
    case led_control_Envelope_scan_ic_tag:
    case led_control_Envelope_set_channel_mode_tag:
    case led_control_Envelope_set_on_off_tag:
    case led_control_Envelope_set_brightness_tag:
    case led_control_Envelope_set_color_tag:
    case led_control_Envelope_set_static_mode_tag:
    case led_control_Envelope_set_preset_effect_tag:
    case led_control_Envelope_scene_op_tag:
    case led_control_Envelope_list_scenes_tag:
    case led_control_Envelope_ping_tag:
    case led_control_Envelope_factory_reset_tag:
        store_response(env.request_id, led_control_ErrorCode_ERR_NOT_SUPPORTED,
                       "not implemented", NULL);
        return;

    default:
        store_response(env.request_id, led_control_ErrorCode_ERR_UNKNOWN,
                       "unknown type", NULL);
        return;
    }
}
