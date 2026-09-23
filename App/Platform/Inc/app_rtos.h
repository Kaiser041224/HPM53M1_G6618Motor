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
 * ------------------------------------------------------------------------- */
#define APP_RTOS_PRIO_BRINGUP           (2)
#define APP_RTOS_PRIO_LOG               (1)
#define APP_RTOS_STACK_BRINGUP_WORDS    (1024)
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
