/**
 * @file    app_rtos_tasks.c
 * @brief   应用任务编排（IO / 诊断任务 + rtt_log；控制环路由 ADC PMT 中断驱动）
 * @author  Kaiser
 *
 * 结构（设计文档：docs/superpowers/specs/2026-09-23-foc-fastlane-task-structure-design.md）：
 *   ADC PMT 中断（25kHz 硬件触发）：FOC 快车道 —— 采样 / 换算 / 保护 / 控制输出
 *                                   （见 app_fast_step，app_logic.c）
 *   app_io  (prio 3, 1ms)  ：app_init → 创建 diag → 慢通道采样 + 调试 + 通讯
 *   app_diag(prio 3, 1s)   ：回报（LED / 统计），不改控制状态
 *   rtt_log (prio 4)       ：日志队列 → SEGGER RTT（见 app_debug_rtt）
 *
 * 实时性契约：25kHz 控制环路由 PWM1 CMP10 → TRGM → ADC0 PMT 硬件触发，
 * 在 PMT 完成中断内执行（ISR 优先级最高，不被任务/日志抢占）。RTOS 任务
 * 仅承担后台域；快车道与后台域只通过无阻塞数据面交换（见设计文档 §3）。
 *
 * 参考 feat/freertos-test-tasks 的步骤函数拆分与单一所有者纪律。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_rtos_tasks.h"

#include <stdbool.h>
#include <stdint.h>

#include "app_debug_rtt.h"
#include "app_rtos.h"

#include "FreeRTOS.h"
#include "task.h"

/* App/Logic 提供的步骤函数（app_logic.c） */
void app_init(void);
void app_io_step(void);
void app_diag_step(void);

static void app_rtos_io_task(void* argument);
static void app_rtos_diag_task(void* argument);

/**
 * @brief IO 任务：一次性初始化 → 创建诊断任务 → 1ms IO 循环。
 * @param argument 未使用
 */
static void app_rtos_io_task(void* argument) {
    (void)argument;

    /* 一次性初始化（电流零点标定依赖 ISR，须在调度器启动后执行） */
    app_init();
    app_rtos_selfcheck_tick();

    if (xTaskCreate(app_rtos_diag_task, "diag", APP_RTOS_STACK_DIAG_WORDS, NULL,
                    APP_RTOS_PRIO_DIAG, NULL) != pdPASS) {
        app_rtos_fatal(__FILE__, __LINE__);
    }

    for (;;) {
        app_io_step();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/**
 * @brief 诊断任务：1s 回报（LED / 统计），不改控制状态。
 * @param argument 未使用
 */
static void app_rtos_diag_task(void* argument) {
    (void)argument;

    for (;;) {
        app_diag_step();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_application_start(void) {
    app_debug_rtt_start_task();

    if (xTaskCreate(app_rtos_io_task, "io", APP_RTOS_STACK_IO_WORDS, NULL, APP_RTOS_PRIO_IO,
                    NULL) != pdPASS) {
        app_rtos_fatal(__FILE__, __LINE__);
    }

    vTaskStartScheduler();

    /* 调度器不应返回 */
    app_rtos_fatal(__FILE__, __LINE__);
}
