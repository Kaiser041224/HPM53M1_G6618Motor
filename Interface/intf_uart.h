/*
 * UART Interface - C17 抽象接口（设备对象 + 匿名结构体）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _INTF_UART_H
#define _INTF_UART_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t intf_uart_port_t;

typedef struct {
    uint32_t baudrate;
    uint8_t  data_bits;
    uint8_t  stop_bits;
    uint8_t  parity;
    bool     flow_ctrl;
} intf_uart_cfg_t;

typedef void (*intf_uart_rx_cb_t)(uint8_t *data, size_t len);

/*
 * UART 设备对象（风格 A：instance_id + 匿名结构体方法）
 * 用法：const intf_uart_t *uart = intf_uart_get(port); uart->transmit(...);
 */
typedef struct {
    uint8_t instance_id; /* 端口实例：0..3 -> UART0..UART3 */
    struct {
        int  (*init)(const intf_uart_cfg_t *cfg);
        int  (*transmit)(const uint8_t *data, size_t len, uint32_t timeout_ms);
        int  (*receive)(uint8_t *data, size_t len, uint32_t timeout_ms);
        int  (*register_rx_callback)(intf_uart_rx_cb_t cb);
        void (*deinit)(void);
    };
} intf_uart_t;

int intf_uart_register(const intf_uart_t *dev);
const intf_uart_t *intf_uart_get(intf_uart_port_t port);

#ifdef __cplusplus
}
#endif

#endif /* _INTF_UART_H */
