/**
 * @file    app_debug_rtt.h
 * @brief   SEGGER RTT 调试输出封装（RTT 写出由 rtt_log 任务调度）
 * @author  Kaiser
 *
 * 设计文档：docs/superpowers/specs/2026-09-23-rtos-foundation-design.md
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_DEBUG_RTT_H
#define APP_DEBUG_RTT_H

#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * 周期调试打印总开关（2026-09-19）
 *   1 = 正常：心跳 / 编码器统计 / CAN 周期行全部输出
 *   0 = 静默：仅保留启动自检与按需命令（d/p/k/n）输出，便于专注观测 ADC
 * 用法：在 CMakeLists.txt 或编译命令行定义 -DAPP_DEBUG_PERIODIC_PRINT=1 打开
 * ============================================================================ */
#ifndef APP_DEBUG_PERIODIC_PRINT
# define APP_DEBUG_PERIODIC_PRINT (0)
#endif

/**
 * @brief 调试输出旁路写入器（附加到 RTT 输出之后调用；NULL = 仅 RTT）。
 * @param text 文本（以 '\0' 结尾）
 * @param len 文本长度（不含 '\0'）
 *
 * 用途：Shell 命令执行期间临时挂接，把既有 dump 输出同时送入终端。
 * 约束：仅在任务上下文挂接/摘除（当前无 ISR 上下文 printf 调用）。
 * 语义：B 起 writer 在生产者上下文**同步**调用（不进队列），
 *       以保持 Terminal capture 的时序。
 */
typedef void (*app_debug_writer_t)(const char* text, size_t len);

/**
 * @brief  格式化输出到 RTT 日志队列（printf 风格）。
 * @param  fmt  格式字符串
 * @param  ...  可变参数
 * @return 已写入的字符数；缓冲不足时返回负值
 *
 * 调度模型（B）：
 *   - 同步调用 writer（若有）；
 *   - 消息入队，由 rtt_log 任务统一写 SEGGER RTT；
 *   - 队列满则整条丢弃并计数，不阻塞生产者（不卡 25kHz 控制环）；
 *   - 队列未启动（start_task 之前）时退化为直写 RTT。
 *
 * 约束：禁止在 ISR 上下文调用（队列操作为任务上下文 API）。
 */
int app_debug_printf(const char* fmt, ...);

/**
 * @brief 设置调试输出旁路写入器（附加，不影响 RTT 输出）。
 * @param writer 写入器；NULL = 摘除
 */
void app_debug_set_writer(app_debug_writer_t writer);

/**
 * @brief 创建 RTT 日志队列 + rtt_log 任务。
 *
 * 须在调度器启动之前调用（main 中、vTaskStartScheduler 之前）。
 * 失败时走 app_rtos_fatal，不返回。
 */
void app_debug_rtt_start_task(void);

/**
 * @brief 获取 RTT 写入成功次数。
 *
 * @return 已成功写入 RTT 上行缓冲的字符串条数。
 */
uint32_t app_debug_rtt_get_write_ok(void);

/**
 * @brief 获取 RTT 写入被丢弃次数。
 * @return 因日志队列满被整条丢弃的字符串条数。
 */
uint32_t app_debug_rtt_get_write_dropped(void);

/**
 * @brief 获取日志队列当前待输出条数。
 * @return 队列深度（0 = 已排空）。
 */
uint32_t app_debug_rtt_get_queue_depth(void);

#endif /* APP_DEBUG_RTT_H */
