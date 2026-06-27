#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "led_control.pb.h"

/**
 * @brief 初始化 OTA 模块（在 app_main 中调用）
 */
void ota_init(void);

/**
 * @brief 处理 OTA 命令帧 (Envelope.ota)
 * @param conn_handle BLE 连接句柄（用于发送响应通知）
 * @param data        Envelope 原始字节（不含 protobuf 封装）
 * @param len         数据长度
 * @note  当 OTA 进行中时传入的裸数据块会直接写入 flash
 */
void ota_handle_data(uint16_t conn_handle, const uint8_t *data, size_t len);

/**
 * @brief 处理 Envelope 中的 OTARequest
 */
void ota_handle_cmd(uint16_t conn_handle, const led_control_OTARequest *req);

/**
 * @brief 通知 OTA 模块连接已断开（用于中止进行中的 OTA）
 */
void ota_on_disconnect(void);

/**
 * @brief 设置 Custom Data 特征值句柄（用于发送通知响应）
 */
void ota_set_data_handle(uint16_t handle);

/**
 * @brief 查询 OTA 是否正在进行
 */
bool ota_is_in_progress(void);
