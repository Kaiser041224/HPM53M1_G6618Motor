/**
 * @file    app_can.h
 * @brief   CAN 平台封装（MCAN 驱动注册、收发与统计）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_CAN_H
#define APP_CAN_H

#include "intf_can.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CAN_RX_BUF_SIZE  (16U)
#define APP_CAN_FILTER_COUNT (16U)

/**
 * @brief CAN 报文
 */
typedef struct {
    uint32_t id;        /**< 报文 ID */
    bool is_ext_id;     /**< 是否扩展帧 */
    uint8_t dlc;        /**< 数据长度 */
    uint8_t data[64];   /**< 数据 */
    uint32_t timestamp; /**< 接收时间戳 */
} app_can_msg_t;

/**
 * @brief CAN 接收回调
 * @param msg 接收到的报文
 */
typedef void (*app_can_rx_callback_t)(const app_can_msg_t* msg);

/**
 * @brief CAN 运行统计
 */
typedef struct {
    uint32_t rx_count;              /**< 接收报文计数 */
    uint32_t rx_irq_count;          /**< 接收中断计数 */
    uint32_t rx_drop_count;         /**< 接收丢弃计数 */
    uint32_t rx_overflow_count;     /**< 接收溢出计数 */
    uint32_t rx_fifo_full_count;    /**< RX FIFO 满计数 */
    uint32_t rx_fifo_lost_count;    /**< RX FIFO 丢失计数 */
    uint32_t tx_enqueue_ok_count;   /**< 发送入队成功计数 */
    uint32_t tx_ok_count;           /**< 发送成功计数 */
    uint32_t tx_fail_count;         /**< 发送失败计数 */
    uint32_t bus_off_count;         /**< Bus-Off 计数 */
    uint32_t error_warning_count;   /**< 错误警告计数 */
    uint32_t error_passive_count;   /**< 错误被动计数 */
    uint32_t protocol_error_count;  /**< 协议错误计数 */
    uint32_t ram_access_fail_count; /**< RAM 访问失败计数 */
    uint32_t last_event_flags;      /**< 最近事件标志 */
    uint32_t last_rx_id;            /**< 最近接收报文 ID */
    uint8_t last_rx_dlc;            /**< 最近接收报文长度 */
    int last_tx_ret;                /**< 最近发送返回值 */
    uint8_t rx_pending_count;       /**< 待处理接收计数 */
    intf_can_status_t last_status;  /**< 最近状态 */
} app_can_stats_t;

/**
 * @brief 注册 CAN 驱动并初始化
 * @return 0 = 成功；-1 = 失败
 */
int app_can_init(void);

/**
 * @brief 仅注册 CAN 驱动（不初始化）
 */
void app_can_register_driver(void);

/**
 * @brief 反初始化 CAN
 */
void app_can_deinit(void);

/**
 * @brief 设置接收回调
 * @param cb 接收回调（NULL = 清除）
 */
void app_can_set_rx_callback(app_can_rx_callback_t cb);

/**
 * @brief 轮询接收与发送队列
 */
void app_can_poll(void);

/**
 * @brief 发送报文（ID 类型按帧内容判定）
 * @param id 报文 ID
 * @param data 数据
 * @param len 数据长度
 * @return 0 = 成功；-1 = 失败
 */
int app_can_send(uint32_t id, const uint8_t* data, uint8_t len);

/**
 * @brief 发送标准帧
 * @param id 标准帧 ID
 * @param data 数据
 * @param len 数据长度
 * @return 0 = 成功；-1 = 失败
 */
int app_can_send_std(uint16_t id, const uint8_t* data, uint8_t len);

/**
 * @brief 发送扩展帧
 * @param id 扩展帧 ID
 * @param data 数据
 * @param len 数据长度
 * @return 0 = 成功；-1 = 失败
 */
int app_can_send_ext(uint32_t id, const uint8_t* data, uint8_t len);

/**
 * @brief 从接收缓冲取一帧报文
 * @param msg 输出报文
 * @return 0 = 成功；-1 = 无数据/失败
 */
int app_can_receive(app_can_msg_t* msg);

/**
 * @brief 添加过滤器（ID 类型按参数判定）
 * @param id 过滤 ID
 * @param mask 过滤掩码
 * @return 0 = 成功；-1 = 失败
 */
int app_can_add_filter(uint32_t id, uint32_t mask);

/**
 * @brief 添加标准帧过滤器
 * @param id 标准帧过滤 ID
 * @param mask 标准帧过滤掩码
 * @return 0 = 成功；-1 = 失败
 */
int app_can_add_std_filter(uint16_t id, uint16_t mask);

/**
 * @brief 添加扩展帧过滤器
 * @param id 扩展帧过滤 ID
 * @param mask 扩展帧过滤掩码
 * @return 0 = 成功；-1 = 失败
 */
int app_can_add_ext_filter(uint32_t id, uint32_t mask);

/**
 * @brief 读取 CAN 状态
 * @param status 输出状态
 * @return 0 = 成功；-1 = 失败
 */
int app_can_get_status(intf_can_status_t* status);

/**
 * @brief 读取运行统计
 * @param stats 输出统计
 * @return 0 = 成功；-1 = 失败
 */
int app_can_get_stats(app_can_stats_t* stats);

/**
 * @brief 清零运行统计
 */
void app_can_clear_stats(void);

/**
 * @brief 当前是否处于 Bus-Off
 * @return true = Bus-Off
 */
bool app_can_is_bus_off(void);

/**
 * @brief 当前 CAN 外设时钟频率（Hz）
 * @return 时钟频率 [Hz]；0 = 未注册/不存在
 */
uint32_t app_can_get_clock_hz(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CAN_H */
