/**
 * @file    main.c
 * @brief   程序入口：板级/时钟初始化 + FreeRTOS 调度（任务编排见 app_rtos_tasks.c）
 * @author  Kaiser
 *
 * 调度结构（设计文档：docs/superpowers/specs/2026-09-23-foc-fastlane-task-structure-design.md）：
 *   main → board_init → intf_clock_init → app_application_start（不返回）
 *   app_io (prio 3, 1ms)  ：app_init → 创建 fast/diag → 慢通道采样 + 调试 + 通讯
 *   app_fast(prio 2, 25kHz)：FOC 快车道（采样 / 换算 / 保护 / 控制输出）
 *   app_diag(prio 3, 1s)   ：回报（LED / 统计）
 *   rtt_log (prio 4)       ：日志队列 → SEGGER RTT
 *
 * 时钟：intf_clock_init() 在调度器启动前完成（对齐 SDK 惯例 board_init 含
 * board_init_clock；保证 MCHTMR tick 基频正确，消除时钟树重配与 tick 的竞争窗口）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "board.h"

#include "app_rtos.h"
#include "app_rtos_tasks.h"
#include "intf_clock.h"

/**
 * @brief  程序入口：板级 + 时钟初始化后进入 FreeRTOS 调度。
 * @return 退出码（正常运行时不会返回）
 */
int main(void) {
    board_init();
    intf_clock_init(); /* CPU 480MHz / AHB 160MHz / MCHTMR 24MHz；调度器前完成 */

    app_application_start();

    /* 调度器不应返回 */
    app_rtos_fatal(__FILE__, __LINE__);
    return 0;
}
