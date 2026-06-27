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

/** 响应缓冲区 — envelope/ota 模块将响应写入此处，blue.c 读取返回给手机 */
extern uint8_t envelope_resp_buf[1024];
extern uint16_t envelope_resp_len;
