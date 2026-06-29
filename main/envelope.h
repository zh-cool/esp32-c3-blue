#pragma once
#include <stdint.h>
#include <stddef.h>

/** 响应缓冲区 — 各组将响应写入此处，blue.c 读取返回给手机 */
extern uint8_t envelope_resp_buf[1024];
extern uint16_t envelope_resp_len;

/**
 * @brief 处理从 Custom Data 接收到的完整 Envelope 数据
 * @param conn_handle BLE 连接句柄
 * @param data        Envelope 原始字节
 * @param len         数据长度
 */
void envelope_handle(uint16_t conn_handle, const uint8_t *data, size_t len);

/** @brief 记录已连接手机句柄 */
void envelope_add_peer(uint16_t conn_handle);

/** @brief 移除已断开手机句柄 */
void envelope_remove_peer(uint16_t conn_handle);

/** @brief 向所有已连接手机发送通知（envelope_resp_buf 的内容） */
void envelope_notify_all(void);

/** @brief 设置 Custom Data 特征值句柄（用于通知） */
void envelope_set_data_handle(uint16_t handle);
