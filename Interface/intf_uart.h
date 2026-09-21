/**
 * @file    intf_uart.h
 * @brief   UART 抽象接口（设备对象 + 匿名结构体）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
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

/**
 * @brief UART 端口实例号
 */
typedef uint8_t intf_uart_port_t;

/**
 * @brief UART 端口配置
 */
typedef struct {
    uint32_t baudrate;  /**< 波特率 [bps] */
    uint8_t  data_bits; /**< 数据位（5..8） */
    uint8_t  stop_bits; /**< 停止位（1..2） */
    uint8_t  parity;    /**< 校验（0=无, 1=奇, 2=偶） */
    bool     flow_ctrl; /**< 是否启用流控 */
} intf_uart_cfg_t;

/**
 * @brief UART 接收回调（中断上下文执行）
 * @param data 接收数据
 * @param len 数据长度
 */
typedef void (*intf_uart_rx_cb_t)(uint8_t *data, size_t len);

/*
 * UART 设备对象（风格 A：instance_id + 匿名结构体方法）
 * 用法：const intf_uart_t *uart = intf_uart_get(port); uart->transmit(...);
 */

/**
 * @brief UART 设备对象
 */
typedef struct {
    uint8_t instance_id; /**< 端口实例：0..3 -> UART0..UART3 */
    struct {
        /**
         * @brief 初始化 UART 端口
         * @param cfg 端口配置
         * @return 0 = 成功；-1 = 失败
         */
        int (*init)(const intf_uart_cfg_t *cfg);

        /**
         * @brief 发送数据
         * @param data 发送缓冲区
         * @param len 数据长度
         * @param timeout_ms 超时（0=不等待，UINT32_MAX=无限，其他=毫秒）
         * @return 0 = 成功；-1 = 失败
         */
        int (*transmit)(const uint8_t *data, size_t len, uint32_t timeout_ms);

        /**
         * @brief 接收数据
         * @param data 接收缓冲区
         * @param len 期望长度
         * @param timeout_ms 超时（0=不等待，UINT32_MAX=无限，其他=毫秒）
         * @return 实际接收长度；-1 = 失败
         */
        int (*receive)(uint8_t *data, size_t len, uint32_t timeout_ms);

        /**
         * @brief 注册接收回调
         * @param cb 回调（NULL = 清除）
         * @return 0 = 成功；-1 = 失败
         */
        int (*register_rx_callback)(intf_uart_rx_cb_t cb);

        /**
         * @brief 反初始化 UART 端口
         */
        void (*deinit)(void);
    };
} intf_uart_t;

/**
 * @brief 注册 UART 设备对象
 * @param dev 设备对象
 * @return 0 = 成功；-1 = 失败
 */
int intf_uart_register(const intf_uart_t *dev);

/**
 * @brief 获取 UART 设备对象
 * @param port 端口实例号
 * @return 设备对象指针；NULL = 未注册/编号越界
 */
const intf_uart_t *intf_uart_get(intf_uart_port_t port);

#ifdef __cplusplus
}
#endif

#endif /* _INTF_UART_H */
