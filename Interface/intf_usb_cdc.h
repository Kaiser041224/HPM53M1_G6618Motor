/*
 * USB CDC (Virtual COM) Interface - C17 抽象接口（设备对象 + 匿名结构体）
 *
 * 虚拟串口抽象层：CDC ACM 设备，语义对齐 intf_uart（写阻塞 + 读非阻塞 + 可选回调）。
 * 本 SoC 单 USB 实例，instance_id 固定为 0。
 *
 * Copyright (c) 2026 HPMicro
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

/* 接收回调（USB 中断上下文执行，data 仅在回调期间有效） */
typedef void (*intf_usb_cdc_rx_cb_t)(const uint8_t *data, size_t len);

/*
 * USB CDC 设备对象（风格 A：instance_id + 匿名结构体方法）
 * 用法：const intf_usb_cdc_t *usb = intf_usb_cdc_get(); usb->write(...);
 */
typedef struct {
    uint8_t instance_id; /* 单实例，= 0 */
    struct {
        int  (*init)(void);
        /* 写：timeout_ms 语义同 drv_uart：0=不等待、UINT32_MAX=无限、其他=毫秒 */
        int  (*write)(const uint8_t *data, size_t len, uint32_t timeout_ms);
        /* 读：非阻塞，返回实际读到的字节数；-1 = 参数/状态错误 */
        int  (*read)(uint8_t *data, size_t len);
        int  (*register_rx_callback)(intf_usb_cdc_rx_cb_t cb);
        /* 上位机已打开虚拟串口（DTR 置位） */
        bool (*is_dtr)(void);
        void (*deinit)(void);
    };
} intf_usb_cdc_t;

int intf_usb_cdc_register(const intf_usb_cdc_t *dev);
const intf_usb_cdc_t *intf_usb_cdc_get(void);

#ifdef __cplusplus
}
#endif

#endif /* INTF_USB_CDC_H */
