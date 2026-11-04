/**
 * @file    intf_usb_bulk.h
 * @brief   USB vendor bulk 通道契约（WinUSB 二进制通道）
 * @author  Kaiser
 *
 * 与 intf_usb_cdc（Terminal 文本通道）并列：本接口只承载二进制协议帧
 * （motor_proto：参数读写、状态快照、后续遥测流）与固件升级传输。
 *
 * 上下文约定：read/write 只允许在任务上下文调用；底层 IN/OUT 回调在 USB
 * 中断上下文执行，仅做环形缓冲搬运。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef INTF_USB_BULK_H
#define INTF_USB_BULK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief bulk 通道运行统计（链路诊断）
 */
typedef struct {
    uint32_t rx_packets; /**< 收到的 OUT 包数 */
    uint32_t tx_packets; /**< 成功排队的 IN 包数 */
    uint32_t rx_drops;   /**< 接收环满丢弃的字节数 */
    uint32_t tx_drops;   /**< IN 忙 / 启动失败 / 超时丢弃的字节数 */
    uint32_t oversize;   /**< 超过单包上限的异常包数 */
    uint32_t bus_resets; /**< 总线复位/断开次数 */
} intf_usb_bulk_stats_t;

/*
 * USB bulk 设备对象（风格 A：instance_id + 匿名结构体方法）
 * 用法：const intf_usb_bulk_t *bulk = intf_usb_bulk_get(); bulk->write(...);
 */

/**
 * @brief USB bulk 设备对象
 */
typedef struct {
    uint8_t instance_id; /**< 实例编号（单实例，= 0） */
    struct {
        /**
         * @brief 初始化（可与 CDC 共用同一次设备初始化，幂等）
         * @return 0 = 成功；-1 = 失败
         */
        int (*init)(void);

        /**
         * @brief 写一帧到 bulk IN 端点（任务上下文；阻塞至完成或超时）
         * @param data 发送缓冲区
         * @param len 数据长度（≤ 底层 TX 缓冲大小）
         * @param timeout_ms 0 = 不等待、UINT32_MAX = 无限、其他 = 毫秒
         * @return 0 = 成功；-1 = 参数/状态错误或超时
         */
        int (*write)(const uint8_t *data, size_t len, uint32_t timeout_ms);

        /**
         * @brief 从接收环取数据（非阻塞）
         * @param data 接收缓冲区
         * @param len 期望长度
         * @return 实际读到的字节数；-1 = 参数/状态错误
         */
        int (*read)(uint8_t *data, size_t len);

        /**
         * @brief 设备是否已枚举完成（可收发）
         * @return true = 已就绪
         */
        bool (*is_ready)(void);

        /**
         * @brief 读取运行统计
         * @param out 统计输出（NULL = 忽略）
         */
        void (*get_stats)(intf_usb_bulk_stats_t *out);
    };
} intf_usb_bulk_t;

/**
 * @brief 注册 USB bulk 设备对象
 * @param dev 设备对象
 * @return 0 = 成功；-1 = 参数非法
 */
int intf_usb_bulk_register(const intf_usb_bulk_t *dev);

/**
 * @brief 获取 USB bulk 设备对象
 * @return 设备对象指针；NULL = 未注册
 */
const intf_usb_bulk_t *intf_usb_bulk_get(void);

#ifdef __cplusplus
}
#endif

#endif /* INTF_USB_BULK_H */
