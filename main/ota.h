#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "led_control.pb.h"

/**
 * @brief 初始化 OTA 模块
 */
void ota_init(void);

/**
 * @brief 处理 Custom Data 收到的完整 Envelope 数据
 * @param conn_handle BLE 连接句柄（用于发送响应通知）
 * @param data        Envelope 原始字节
 * @param len         数据长度
 */
void ota_handle_envelope(uint16_t conn_handle, const uint8_t *data, size_t len);

/**
 * @brief 连接断开通知
 */
void ota_on_disconnect(void);

/**
 * @brief 设置 Custom Data 特征值句柄（用于发送响应通知）
 */
void ota_set_data_handle(uint16_t handle);
