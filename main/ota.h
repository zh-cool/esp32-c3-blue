#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief 初始化 OTA 模块
 */
void ota_init(void);

/**
 * @brief 处理 Envelope 中的 OTARequest（原始 Envelope 字节）
 * @param data  Envelope 原始字节
 * @param len   数据长度
 * @note  响应写入 envelope_resp_buf，由对端读取
 */
void ota_handle_envelope(const uint8_t *data, size_t len);

/**
 * @brief 断开连接时中止 OTA
 */
void ota_on_disconnect(void);
