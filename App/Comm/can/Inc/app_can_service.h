/**
 * @file    app_can_service.h
 * @brief   CAN 控制服务（1kHz 编排：状态机 / 队列 / 看门狗 / 周期发送）
 * @author  Kaiser
 *
 * 分层：app_can（平台收发）→ app_can_protocol（纯编解码）→ 本模块（编排）。
 * 执行模型（不可回退）：
 *   - RX 中断只做两件事：喂狗时间戳（控制帧）+ 入队；零协议逻辑
 *   - 协议解析/命令执行在 1kHz io 任务，有界（每拍 ≤8 帧）
 *   - 状态转换一律非阻塞请求，禁止在 handler 内调用阻塞式停机
 *   - 停机窗口内队列溢出丢帧可接受，但必须计数；恢复时清队
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

/** @brief 初始化（注册 RX 回调与 ISR 钩子、过滤器；不重复初始化外设） */
void app_can_service_init(void);

/** @brief 1kHz 步进：收队列 → 执行 → 看门狗 → 周期发送 */
void app_can_service_step(void);

/**
 * @brief 挂起/恢复通信看门狗
 * @param suspend true = 挂起（CALIB / 主动停机窗口内调用）
 */
void app_can_service_suspend_watchdog(bool suspend);

/**
 * @brief 由非 CAN 源喂通信看门狗（当前：USB 终端输入）
 *
 * 语义：通信看门狗默认由 CAN 控制帧（RX ISR）喂；USB 终端有输入时同样喂，
 * 且优先级更高——人在终端操作期间不会因 CAN 静默被判"主机失联"。
 * 两个源共享同一时间戳与超时判据，后到者刷新。
 *
 * 可在任务上下文调用（不加锁；时间戳写入和 ISR 侧一致，单字写入天然原子）。
 */
void app_can_service_feed_watchdog(void);

/** @brief 看门狗是否已 arm（收到首个有效控制帧后为 true） */
bool app_can_service_watchdog_armed(void);

/**
 * @brief 看门狗完整状态（诊断）
 * @param armed 是否已 arm（可为 NULL）
 * @param suspended 是否被挂起（CALIB/停机窗口）
 * @param fired 是否已超时触发
 */
void app_can_service_get_watchdog(bool* armed, bool* suspended, bool* fired);

/** @brief 清空接收队列（状态转 IDLE / 停机完成时调用） */
void app_can_service_flush_rx(void);

/** @brief 本节点号（1..15） */
uint8_t app_can_service_get_node_id(void);

/** @brief 接收队列溢出丢帧计数（诊断） */
uint32_t app_can_service_get_dropped(void);

/** @brief 协议统计（诊断；可为 NULL） */
typedef struct {
    uint32_t rx_frames;       /**< 已解析控制帧 */
    uint32_t rx_dropped;      /**< 队列溢出 */
    uint32_t bad_len;         /**< 长度不足/非法帧 */
    uint32_t unknown_cmd;     /**< 未知命令 */
    uint32_t format_mismatch; /**< MIT 报文格式与配置不符 */
    uint32_t mit_pair_ok;     /**< dual 配对成功 */
    uint32_t mit_pair_timeout;/**< dual 配对超时 */
} app_can_service_stats_t;
void app_can_service_get_stats(app_can_service_stats_t* out);

#ifdef __cplusplus
}
#endif

#endif /* APP_CAN_SERVICE_H */