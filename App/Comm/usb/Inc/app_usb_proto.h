/**
 * @file    app_usb_proto.h
 * @brief   USB bulk 二进制协议服务（motor_proto v1.3，P1a 只读子集）
 * @author  Kaiser
 *
 * 链路：WinUSB bulk IN/OUT（Interface 0），与 CDC Terminal 相互独立。
 * 生命周期：init() 在 app_init() 末尾（USB 初始化之后）调用；
 *           service() 在 app_io_step() 1kHz 慢任务中轮询，ISR 只搬运字节。
 *
 * P1a 支持：PROTO_INFO / PARAM_META_COUNT / PARAM_META_GET / PARAM_READ /
 *           STATUS_GET / FAULT_GET / FAULT_CLEAR。
 * P1b 待补：PARAM_WRITE / PARAM_COMMIT / PARAM_RESET / CTRL_CMD。
 * 遥测流（STREAM_*）留给 P2。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef APP_USB_PROTO_H
#define APP_USB_PROTO_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化协议服务（复位解码状态；幂等）
 */
void app_usb_proto_init(void);

/**
 * @brief 协议服务单步（1kHz 慢任务；单次处理有界）
 */
void app_usb_proto_service(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_USB_PROTO_H */
