/**
 * @file    app_rtos.h
 * @brief   FreeRTOS 应用侧服务：致命钩子 + tick 基频自检 + 任务预算常量
 * @author  Kaiser
 *
 * 设计文档：docs/superpowers/specs/2026-09-23-rtos-foundation-design.md
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_RTOS_H
#define APP_RTOS_H

/* ---------------------------------------------------------------------------
 * 任务预算（栈单位：word）
 *   A：bring-up 任务（app_init + app_run 超循环）
 *   B：rtt_log 任务（RTT 输出由 RTOS 调度）
 *
 * 优先级说明：app 超循环忙等不阻塞，低优先级 logger 会被饿死，
 * 故 rtt_log 优先级高于 app；无日志时 log 阻塞在队列上，零打扰。
 *
 * 栈说明：app 任务 2048 words = 8KB。裸机时 app_init 跑在 16KB 主栈上；
 * FreeRTOS 下首层中断帧（含 FPU 约 300B）还压在任务栈上，4KB 不足，
 * 栈溢出曾砸穿 ucHeap 邻块（mepc 落在栈区是其指纹）。8KB 留足余量。
 * ------------------------------------------------------------------------- */
#define APP_RTOS_PRIO_BRINGUP           (2)
#define APP_RTOS_PRIO_LOG               (3)
#define APP_RTOS_STACK_BRINGUP_WORDS    (2048)
#define APP_RTOS_STACK_LOG_WORDS        (512)

/**
 * @brief 致命错误处理：RTT 直写报错 → 三相紧急关断 → 关中断死循环。
 * @param file 触发文件名（configASSERT 传 __FILE__）
 * @param line 触发行号（configASSERT 传 __LINE__）
 *
 * 说明：不走日志队列，保证在 log 任务未启动/已卡死时仍能输出。
 *       不返回。
 */
void app_rtos_fatal(const char* file, long line);

/**
 * @brief tick 基频自检：用 mcycle 实测 32 个 RTOS tick 的平均周期，与 1ms 比对。
 *
 * 须在调度器启动之后调用（tick 已在走）。结果经 app_debug_printf 输出：
 *   tick_selfcheck: PASS|FAIL, avg=x.x us, dev=x.x% (n=32)
 * 偏差 >2% 判 FAIL（仅报错，不阻断后续运行，便于继续观测）。
 */
void app_rtos_selfcheck_tick(void);

#endif /* APP_RTOS_H */
