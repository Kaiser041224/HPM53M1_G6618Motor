/*
 * USB CDC (Virtual COM) Interface - C17 Abstract Interface
 *
 * 虚拟串口抽象层：CDC ACM 设备，语义对齐 intf_uart（写阻塞 + 读非阻塞 + 可选回调）。
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

typedef struct {
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
} intf_usb_cdc_ops_t;

int  intf_usb_cdc_register(const intf_usb_cdc_ops_t *ops);
int  intf_usb_cdc_init(void);
int  intf_usb_cdc_write(const uint8_t *data, size_t len, uint32_t timeout_ms);
int  intf_usb_cdc_read(uint8_t *data, size_t len);
int  intf_usb_cdc_register_rx_callback(intf_usb_cdc_rx_cb_t cb);
bool intf_usb_cdc_is_dtr(void);

#ifdef __cplusplus
}
#endif

#endif /* INTF_USB_CDC_H */
