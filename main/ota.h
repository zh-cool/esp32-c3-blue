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
 * @brief 处理 OTA 命令
 * @param req    已解码的 OTARequest
 * @param req_id 请求 ID（原样返回响应）
 * @note  响应写入 envelope_resp_buf，由对端读取
 */
void ota_handle_cmd(const led_control_OTARequest *req, uint32_t req_id, uint16_t conn_handle);

/**
 * @brief 断开连接时中止 OTA
 */
void ota_on_disconnect(void);
