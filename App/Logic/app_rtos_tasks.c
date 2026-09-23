/**
 * @file    app_rtos_tasks.c
 * @brief   应用任务编排（FOC 快车道 / IO / 诊断三任务 + rtt_log）
 * @author  Kaiser
 *
 * 结构（设计文档：docs/superpowers/specs/2026-09-23-foc-fastlane-task-structure-design.md）：
 *   app_io  (prio 3, 1ms)  ：app_init → 创建 fast/diag → 慢通道采样 + 调试 + 通讯
 *   app_fast(prio 2, 25kHz)：FOC 快车道 —— 采样 / 换算 / 保护 / 控制输出
 *   app_diag(prio 3, 1s)   ：回报（LED / 统计），不改控制状态
 *   rtt_log (prio 4)       ：日志队列 → SEGGER RTT（见 app_debug_rtt）
 *
 * 调度机制：app_fast 忙等超循环只饿死同/低优先级；app_io / app_diag 用
 * vTaskDelay 阻塞后由 tick（1kHz）唤醒并抢占 app_fast（prio 3 > 2），
 * 形成「快车道独占 CPU + 慢任务按需短抢占」的 FOC 期望结构。
 *
 * 参考 feat/freertos-test-tasks 的步骤函数拆分与单一所有者纪律；
 * 改进其"批处理过渡快车道"为精确 25kHz mcycle 节拍（FOC 硬实时要求）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_rtos_tasks.h"

#include <stdbool.h>
#include <stdint.h>

#include "app_debug_encoder.h"
#include "app_debug_rtt.h"
#include "app_hardware_params.h"
#include "app_rtos.h"
#include "intf_clock.h"

#include "FreeRTOS.h"
#include "task.h"

/* App/Logic 提供的步骤函数（app_logic.c） */
void app_init(void);
void app_fast_step(void);
void app_io_step(void);
void app_diag_step(void);

static void app_rtos_fast_task(void* argument);
static void app_rtos_io_task(void* argument);
static void app_rtos_diag_task(void* argument);

/**
 * @brief FOC 快车道任务：25kHz mcycle 忙等超循环（精确节拍）。
 *
 * 硬约束：本任务体内无 RTOS API（除循环外层的创建路径）、无 printf、无等待；
 * 节拍对齐用 mcycle 忙等（FreeRTOS tick 不得作为 FOC 触发源）。
 * @param argument 未使用
 */
static void app_rtos_fast_task(void* argument) {
    const app_hardware_params_t* hardware = app_hardware_params_current();
    const uint32_t cpu_freq = intf_clock_get_cpu_freq();
    const uint32_t loop_cycles = cpu_freq / hardware->inverter.pwm_freq_hz;
    uint32_t next = intf_clock_get_cycle() + loop_cycles;

    (void)argument;

    for (;;) {
        uint32_t now = intf_clock_get_cycle();
        int32_t late = (int32_t)(now - next);

        /* 迟到计数并重同步（心跳/打印已迁至 app_diag，不污染指标） */
        if (late >= 0) {
            app_debug_encoder_note_loop_late((uint32_t)late);
            next = now + loop_cycles;
        }

        app_fast_step();

        /* 忙等对齐下一节拍（不 yield，不阻塞） */
        while ((int32_t)(intf_clock_get_cycle() - next) < 0) {
        }
        next += loop_cycles;
    }
}

/**
 * @brief IO 任务：一次性初始化 → 创建快车道/诊断任务 → 1ms IO 循环。
 * @param argument 未使用
 */
static void app_rtos_io_task(void* argument) {
    (void)argument;

    /* 一次性初始化（电流零点标定依赖 ISR，须在调度器启动后执行） */
    app_init();
    app_rtos_selfcheck_tick();

    if (xTaskCreate(app_rtos_fast_task, "fast", APP_RTOS_STACK_FAST_WORDS, NULL,
                    APP_RTOS_PRIO_FAST, NULL) != pdPASS) {
        app_rtos_fatal(__FILE__, __LINE__);
    }
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
