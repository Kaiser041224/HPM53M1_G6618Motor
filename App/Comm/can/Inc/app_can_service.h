/**
 * @file    app_can_service.h
 * @brief   DM 兼容 CAN 运行时服务（1kHz 编排：解析 → 执行 → 回帧）
 * @author  Kaiser
 *
 * 分层：app_can（平台收发）→ app_can_protocol（纯编解码）→ app_can_policy（动作规划）
 *       → 本模块（执行/计数/看门狗/反馈）。
 *
 * 执行模型（不可回退）：
 *   - RX 中断只做两件事：喂狗时间戳（本机控制类帧）+ 入队；零协议逻辑
 *   - 解析/执行/回帧在 1kHz io 任务，有界（每拍 ≤8 帧、≤8 回帧）
 *   - 反馈为问询式：每收到一条通过校验的本机帧回一帧
 *   - 模式切换：运动进行中收到不同运动模式的帧 → 释放输出并丢弃本帧，下一拍接受
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_CAN_SERVICE_H
#define APP_CAN_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化（注册 RX 回调与 ISR 钩子、过滤器；不重复初始化外设）
 * @param node_id 本机节点号（0..15；越界回退 1）
 */
void app_can_service_init(uint8_t node_id);

/** @brief 1kHz 步进：收队列 → 执行 → 回帧 → 看门狗 */
void app_can_service_step(void);

/**
 * @brief 挂起/恢复通信看门狗
 * @param suspend true = 挂起（CALIB / 主动停机窗口内调用）
 */
void app_can_service_suspend_watchdog(bool suspend);

/**
 * @brief 由非 CAN 源喂通信看门狗（当前：USB 终端输入）
 *
 * 语义：CAN 控制帧（RX ISR）与 USB 终端输入共享同一时间戳与超时判据，
 * 后到者刷新；任一源活跃即保持。
 */
void app_can_service_feed_watchdog(void);

/** @brief 看门狗是否已 arm（收到首个有效控制帧后为 true） */
bool app_can_service_watchdog_armed(void);

/** @brief 看门狗完整状态（诊断；任一输出可为 NULL） */
void app_can_service_get_watchdog(bool* armed, bool* suspended, bool* fired);

/** @brief 清空接收队列（停机/恢复时调用） */
void app_can_service_flush_rx(void);

/** @brief 本机节点号（0..15） */
uint8_t app_can_service_get_node_id(void);

/** @brief 接收队列溢出丢帧计数（诊断） */
uint32_t app_can_service_get_dropped(void);

/** @brief 协议/服务统计（诊断） */
typedef struct {
    uint32_t rx_frames;  /**< 通过校验的本机帧 */
    uint32_t rx_dropped; /**< 队列溢出丢帧 */
    uint32_t bad_frames; /**< 长度/magic/非有限值非法 */
    uint32_t foreign;    /**< 非本机或未知类型 */
    uint32_t mode_switch;/**< 运行中跨模式触发释放输出 */
    uint32_t rejected;   /**< 执行被 motion/FOC/故障模块拒绝 */
    uint32_t tx_fail;    /**< 反馈发送失败 */
} app_can_service_stats_t;

void app_can_service_get_stats(app_can_service_stats_t* out);

#ifdef __cplusplus
}
#endif

#endif /* APP_CAN_SERVICE_H */
