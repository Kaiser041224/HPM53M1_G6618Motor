/**
 * @file    app_rtos.c
 * @brief   FreeRTOS 应用侧服务：致命钩子 + tick 基频自检
 * @author  Kaiser
 *
 * 设计文档：docs/superpowers/specs/2026-09-23-rtos-foundation-design.md
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_rtos.h"

#include <stdbool.h>
#include <stdint.h>

#include "app_3phase_inverter.h"
#include "app_debug_rtt.h"
#include "intf_clock.h"

#include "FreeRTOS.h"
#include "SEGGER_RTT.h"
#include "task.h"

/** tick 自检采样个数 */
#define APP_RTOS_TICK_SELFCHECK_SAMPLES (32U)
/** tick 自检允许偏差（0.1% 单位；20 = 2.0%） */
#define APP_RTOS_TICK_SELFCHECK_DEV_MAX (20U)

/**
 * @brief 致命错误处理：RTT 直写 → 紧急关断 → 停机。
 * @param file 触发文件名
 * @param line 触发行号
 */
void app_rtos_fatal(const char* file, long line) {
    /* 直写 RTT，不经日志队列（队列/生产者可能已损坏） */
    SEGGER_RTT_printf(0, "\r\n[FATAL] app_rtos: %s:%ld\r\n", (file != NULL) ? file : "?", line);

    app_3phase_inverter_emergency_stop();

    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

/**
 * @brief 栈溢出钩子（configCHECK_FOR_STACK_OVERFLOW = 2）。
 * @param task  任务句柄
 * @param name  任务名
 */
void vApplicationStackOverflowHook(TaskHandle_t task, char* name) {
    (void)task;
    SEGGER_RTT_WriteString(0, "\r\n[FATAL] stack overflow: ");
    SEGGER_RTT_WriteString(0, (name != NULL) ? name : "?");
    SEGGER_RTT_WriteString(0, "\r\n");
    app_rtos_fatal(__FILE__, __LINE__);
}

/**
 * @brief malloc 失败钩子（configUSE_MALLOC_FAILED_HOOK = 1）。
 */
void vApplicationMallocFailedHook(void) {
    SEGGER_RTT_WriteString(0, "\r\n[FATAL] pvPortMalloc failed\r\n");
    app_rtos_fatal(__FILE__, __LINE__);
}

/**
 * @brief tick 基频自检（mcycle 实测 32 tick 平均周期 vs 1ms）。
 */
void app_rtos_selfcheck_tick(void) {
    const uint32_t samples = APP_RTOS_TICK_SELFCHECK_SAMPLES;
    const uint32_t expected_deci_us = 10000U; /* 1000.0 us */
    uint32_t cpu_freq;
    uint32_t start_cycle;
    uint32_t elapsed_cycles;
    uint32_t start_tick;
    uint32_t avg_deci_us;
    uint32_t dev_deci_us;
    uint32_t dev_pct_x10;
    bool pass;

    cpu_freq = intf_clock_get_cpu_freq();
    if (cpu_freq == 0U) {
        app_debug_printf("tick_selfcheck: FAIL, cpu_freq=0\r\n");
        return;
    }

    start_cycle = intf_clock_get_cycle();
    start_tick = xTaskGetTickCount();
    while ((uint32_t)(xTaskGetTickCount() - start_tick) < samples) {
        /* 等待 tick 推进 */
    }
    elapsed_cycles = intf_clock_get_cycle() - start_cycle;

    /* 平均周期，0.1us 单位：elapsed * 10^7 / (cpu_freq * samples) */
    avg_deci_us = (uint32_t)(((uint64_t)elapsed_cycles * 10000000ULL) /
                             ((uint64_t)cpu_freq * (uint64_t)samples));
    dev_deci_us = (avg_deci_us > expected_deci_us) ? (avg_deci_us - expected_deci_us) :
                                                    (expected_deci_us - avg_deci_us);
    dev_pct_x10 = (uint32_t)(((uint64_t)dev_deci_us * 1000ULL) / (uint64_t)expected_deci_us);
    pass = (dev_pct_x10 <= APP_RTOS_TICK_SELFCHECK_DEV_MAX);

    app_debug_printf(
        "tick_selfcheck: %s, avg=%u.%u us, dev=%u.%u%% (n=%u)\r\n", pass ? "PASS" : "FAIL",
        (unsigned)(avg_deci_us / 10U), (unsigned)(avg_deci_us % 10U),
        (unsigned)(dev_pct_x10 / 10U), (unsigned)(dev_pct_x10 % 10U), (unsigned)samples);
}
