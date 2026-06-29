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

/* ======================== 存入响应缓冲区 ======================== */

static void store_resp(uint32_t request_id, led_control_ErrorCode error,
                       const char *msg, led_control_EnvelopeResponse *resp)
{
    led_control_Envelope env = led_control_Envelope_init_default;
    env.protocol_version = 2;
    env.request_id = request_id;
    env.which_payload = led_control_Envelope_response_tag;

    if (resp) {
        env.payload.response = *resp;
        /* 确保错误消息是字符串拷贝而非回调 */
    } else {
        env.payload.response.request_id = request_id;
        env.payload.response.error = error;
        if (msg) strncpy(env.payload.response.error_msg, msg, sizeof(env.payload.response.error_msg) - 1);
    }

    pb_ostream_t s = pb_ostream_from_buffer(envelope_resp_buf, sizeof(envelope_resp_buf));
    if (pb_encode(&s, led_control_Envelope_fields, &env))
        envelope_resp_len = s.bytes_written;
    else
        ESP_LOGE(TAG, "encode fail: %s", PB_GET_ERROR(&s));
}

/* ======================== GetDeviceInfo ======================== */

static void handle_get_device_info(uint32_t req_id)
{
    led_control_EnvelopeResponse r = led_control_EnvelopeResponse_init_default;
    r.request_id = req_id;
    r.error = led_control_ErrorCode_OK;
    r.which_result = led_control_EnvelopeResponse_device_info_result_tag;
    strncpy(r.result.device_info_result.info.fw_version, "1.0.0",
            sizeof(r.result.device_info_result.info.fw_version) - 1);
    store_resp(req_id, led_control_ErrorCode_OK, NULL, &r);
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
        ota_handle_envelope(data, len);
        return;

    case led_control_Envelope_get_device_info_tag:
        handle_get_device_info(env.request_id);
        return;

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
        store_resp(env.request_id, led_control_ErrorCode_ERR_NOT_SUPPORTED,
                   "not implemented", NULL);
        return;

    default:
        store_resp(env.request_id, led_control_ErrorCode_ERR_UNKNOWN,
                   "unknown type", NULL);
        return;
    }
}
