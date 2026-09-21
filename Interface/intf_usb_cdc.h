/**
 * @file    intf_usb_cdc.h
 * @brief   USB CDC（虚拟串口）抽象接口（设备对象 + 匿名结构体）
 * @author  Kaiser
 *
 * 虚拟串口抽象层：CDC ACM 设备，语义对齐 intf_uart（写阻塞 + 读非阻塞 + 可选回调）。
 * 本 SoC 单 USB 实例，instance_id 固定为 0。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef INTF_USB_CDC_H
#define INTF_USB_CDC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief USB CDC 接收回调（USB 中断上下文执行，data 仅在回调期间有效）
 * @param data 接收数据
 * @param len 数据长度
 */
typedef void (*intf_usb_cdc_rx_cb_t)(const uint8_t *data, size_t len);

/*
 * USB CDC 设备对象（风格 A：instance_id + 匿名结构体方法）
 * 用法：const intf_usb_cdc_t *usb = intf_usb_cdc_get(); usb->write(...);
 */

/**
 * @brief USB CDC 设备对象
 */
typedef struct {
    uint8_t instance_id; /**< 实例编号（单实例，= 0） */
    struct {
        /**
         * @brief 初始化 USB CDC 设备
         * @return 0 = 成功；-1 = 失败
         */
        int (*init)(void);

        /**
         * @brief 写数据（timeout_ms 语义同 drv_uart：0=不等待、UINT32_MAX=无限、其他=毫秒）
         * @param data 发送缓冲区
         * @param len 数据长度
         * @param timeout_ms 超时 [ms]
         * @return 0 = 成功；-1 = 失败
         */
        int (*write)(const uint8_t *data, size_t len, uint32_t timeout_ms);

        /**
         * @brief 读数据（非阻塞）
         * @param data 接收缓冲区
         * @param len 期望长度
         * @return 实际读到的字节数；-1 = 参数/状态错误
         */
        int (*read)(uint8_t *data, size_t len);

        /**
         * @brief 注册接收回调
         * @param cb 回调（NULL = 清除）
         * @return 0 = 成功；-1 = 失败
         */
        int (*register_rx_callback)(intf_usb_cdc_rx_cb_t cb);

        /**
         * @brief 上位机是否已打开虚拟串口（DTR 置位）
         * @return true = 已打开
         */
        bool (*is_dtr)(void);

        /**
         * @brief 反初始化 USB CDC 设备
         */
        void (*deinit)(void);
    };
} intf_usb_cdc_t;

/**
 * @brief 注册 USB CDC 设备对象
 * @param dev 设备对象
 * @return 0 = 成功；-1 = 失败
 */
int intf_usb_cdc_register(const intf_usb_cdc_t *dev);

/**
 * @brief 获取 USB CDC 设备对象
 * @return 设备对象指针；NULL = 未注册
 */
const intf_usb_cdc_t *intf_usb_cdc_get(void);

#ifdef __cplusplus
}
#endif

#endif /* INTF_USB_CDC_H */
