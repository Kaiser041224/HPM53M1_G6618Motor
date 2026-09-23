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
 *
 * 优先级（高 → 低）：
 *   rtt_log(4) > app_io(3) = app_diag(3) > app_fast(2) > idle(0)
 *
 * 调度机制：app_fast 为 25kHz mcycle 忙等超循环（不 yield），只饿死同/低优先级；
 * app_io / app_diag 用 vTaskDelay 阻塞后由 tick（1kHz）唤醒并**抢占** app_fast，
 * 形成「快车道独占 CPU + 慢任务按需短抢占」的 FOC 期望结构。rtt_log 阻塞在
 * 日志队列上，无日志零打扰。
 *
 * 栈说明：app_fast 2048 words = 8KB。裸机时 app_init 跑在 16KB 主栈上；
 * FreeRTOS 下首层中断帧（含 FPU 约 300B）压在任务栈上（portContext.h 先 SAVE
 * 再切 ISR 栈）。指纹：mepc 落在 ucHeap 区间 = 控制流被栈砸烂。
 * ------------------------------------------------------------------------- */
#define APP_RTOS_PRIO_FAST              (2)
#define APP_RTOS_PRIO_IO                (3)
#define APP_RTOS_PRIO_DIAG              (3)
#define APP_RTOS_PRIO_LOG               (4)

#define APP_RTOS_STACK_FAST_WORDS       (2048)  /* 8KB：快车道 + 中断帧余量 */
#define APP_RTOS_STACK_IO_WORDS         (1536)  /* 6KB：init/printf/Terminal */
#define APP_RTOS_STACK_DIAG_WORDS       (512)   /* 2KB：printf */
#define APP_RTOS_STACK_LOG_WORDS        (512)   /* 2KB：队列排空写 RTT */

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
