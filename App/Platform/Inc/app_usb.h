/**
 * @file    app_usb.h
 * @brief   USB CDC 虚拟串口平台封装（USB0，HS）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_USB_H
#define APP_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册 USB CDC 驱动并初始化为虚拟串口（USB0，HS）。
 */
void app_usb_init(void);

/**
 * @brief 发送数据（100ms 超时；未枚举/未打开端口时返回 -1）。
 * @param data 待发送数据
 * @param len 数据长度
 * @return 0 成功，-1 失败
 */
int app_usb_write(const uint8_t* data, size_t len);

/**
 * @brief 发送数据（可指定超时；timeout_ms=0 = 非阻塞，端点忙时立即返回 -1）。
 *
 * 用于调试通道的周期输出：主机未打开端口时不会阻塞调用方（控制环）。
 * @param data 待发送数据
 * @param len 数据长度
 * @param timeout_ms 超时 [ms]
 * @return 0 成功（已排队/已发出），-1 失败
 */
int app_usb_write_timeout(const uint8_t* data, size_t len, uint32_t timeout_ms);

/**
 * @brief 发送以 '\0' 结尾的字符串。
 * @param str 待发送字符串
 * @return 0 成功，-1 失败
 */
int app_usb_write_str(const char* str);

/**
 * @brief 读取接收数据（非阻塞）。
 * @param data 接收缓冲
 * @param len 期望长度
 * @return 实际读到的字节数（≥0），-1 = 参数/状态错误
 */
int app_usb_read(uint8_t* data, size_t len);

/**
 * @brief 上位机是否已打开虚拟串口（DTR 置位）。
 * @return true = 已打开
 */
bool app_usb_is_dtr(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_USB_H */
