#pragma once
#include <stdint.h>
#include <stddef.h>

/**
 * @brief 处理从 Custom Data 接收到的完整 Envelope 数据
 * @param conn_handle BLE 连接句柄（用于响应通知）
 * @param data        Envelope 原始字节
 * @param len         数据长度
 */
void envelope_handle(uint16_t conn_handle, const uint8_t *data, size_t len);

/**
 * @brief 设置 Custom Data 特征值句柄（用于发送通知响应）
 */
void envelope_set_data_handle(uint16_t handle);
