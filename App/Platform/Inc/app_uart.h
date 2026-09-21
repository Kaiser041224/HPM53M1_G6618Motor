/**
 * @file    app_uart.h
 * @brief   UART 平台封装（控制台上位机调参 + ISP）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_UART_H
#define APP_UART_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UART0：PA00=TXD / PA01=RXD（J4：pin1=RX / pin2=TX / pin3=GND），上位机调参 + ISP */
#define APP_UART_PORT_CONSOLE (0U)

/**
 * @brief 注册 UART 驱动并初始化控制台端口（115200 8N1，中断 RX）。
 * @return 0 = 成功；-1 = 设备未注册或初始化失败
 */
int app_uart_init(void);

/**
 * @brief 阻塞发送（100ms 超时）。
 * @param data 待发送数据
 * @param len 数据长度
 * @return 0 成功，-1 失败
 */
int app_uart_write(const uint8_t* data, size_t len);

/**
 * @brief 发送以 '\0' 结尾的字符串。
 * @param str 待发送字符串
 * @return 0 成功，-1 失败
 */
int app_uart_write_str(const char* str);

/**
 * @brief 读取接收数据（timeout_ms 语义同驱动：0=不等待、UINT32_MAX=无限、其他=毫秒）。
 * @param data 接收缓冲
 * @param len 期望长度
 * @param timeout_ms 超时 [ms]
 * @return 实际读到的字节数（≥0），-1 = 参数/状态错误
 */
int app_uart_read(uint8_t* data, size_t len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* APP_UART_H */
