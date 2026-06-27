#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

/* NimBLE */
#include "host/ble_hs.h"
#include "host/ble_gatt.h"

/* Protobuf */
#include "pb_encode.h"
#include "pb_decode.h"
#include "led_control.pb.h"

#include "envelope.h"
#include "ota.h"

static const char *TAG = "ENV";

/* Custom Data 特征值句柄 — 用于发送通知响应 */
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

static void send_error(uint16_t conn_handle, uint32_t request_id,
                       led_control_ErrorCode error, const char *msg)
{
    led_control_EnvelopeResponse resp = led_control_EnvelopeResponse_init_default;
    resp.request_id = request_id;
    resp.error = error;
    resp.error_msg.arg = (void *)(msg ? msg : "");
    resp.error_msg.funcs.encode = str_cb;
    /* 无 result — 纯错误响应 */

    uint8_t buf[256];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));

    led_control_Envelope env = led_control_Envelope_init_default;
    env.protocol_version = 2;
    env.request_id = request_id;
    env.which_payload = led_control_Envelope_response_tag;
    env.payload.response = resp;

    if (pb_encode(&stream, led_control_Envelope_fields, &env))
        send_notify(conn_handle, buf, stream.bytes_written);
    else
        ESP_LOGE(TAG, "encode error: %s", PB_GET_ERROR(&stream));
}

static void send_ok(uint16_t conn_handle, uint32_t request_id,
                    led_control_ErrorCode error, const char *msg)
{
    send_error(conn_handle, request_id, error, msg);
}

/* ======================== 各消息类型处理 ======================== */

static void handle_get_device_info(uint16_t conn_handle, uint32_t req_id)
{
    /* TODO: 返回真实设备信息 */
    led_control_GetDeviceInfoResponse dev_resp = led_control_GetDeviceInfoResponse_init_default;
    led_control_DeviceInfo info = led_control_DeviceInfo_init_default;
    info.fw_version = (pb_callback_t){.funcs.encode = str_cb, .arg = (void *)"1.0.0"};
    dev_resp.info = info;

    led_control_EnvelopeResponse resp = led_control_EnvelopeResponse_init_default;
    resp.request_id = req_id;
    resp.error = led_control_ErrorCode_OK;
    resp.which_result = led_control_EnvelopeResponse_device_info_result_tag;
    resp.result.device_info_result = dev_resp;

    uint8_t buf[256];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    led_control_Envelope env = led_control_Envelope_init_default;
    env.protocol_version = 2;
    env.request_id = req_id;
    env.which_payload = led_control_Envelope_response_tag;
    env.payload.response = resp;

    if (pb_encode(&stream, led_control_Envelope_fields, &env))
        send_notify(conn_handle, buf, stream.bytes_written);
}

/* ======================== 主入口 ======================== */

void envelope_handle(uint16_t conn_handle, const uint8_t *data, size_t len)
{
    pb_istream_t stream = pb_istream_from_buffer(data, len);
    led_control_Envelope env = led_control_Envelope_init_default;

    if (!pb_decode(&stream, led_control_Envelope_fields, &env)) {
        ESP_LOGW(TAG, "decode fail: %s", PB_GET_ERROR(&stream));
        return;
    }

    ESP_LOGI(TAG, "type=%d, req=%u", env.which_payload, env.request_id);

    switch (env.which_payload) {

    /* ---- OTA ---- */
    case led_control_Envelope_ota_tag:
        ota_handle_envelope(conn_handle, data, len);
        return;

    /* ---- 查询 ---- */
    case led_control_Envelope_get_device_info_tag:
        handle_get_device_info(conn_handle, env.request_id);
        return;

    /* ---- 其他 -- 暂不支持 ---- */
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
        send_error(conn_handle, env.request_id,
                   led_control_ErrorCode_ERR_NOT_SUPPORTED, "not implemented");
        return;

    default:
        ESP_LOGW(TAG, "unknown type=%d", env.which_payload);
        send_error(conn_handle, env.request_id,
                   led_control_ErrorCode_ERR_UNKNOWN, "unknown message type");
        return;
    }
}

/* ======================== 初始化 ======================== */

void envelope_set_data_handle(uint16_t handle)
{
    s_data_handle = handle;
}
