/**
 * @file    app_debug_profiler.c
 * @brief   CPU 周期占用分析输出
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_debug_profiler.h"

#include "app_debug_rtt.h"
#include "irq_profiler.h"

#include <stdint.h>

/**
 * @brief 由事件增量与窗口周期数计算平均频率
 * @param delta 窗口内事件增量
 * @param elapsed_cycles 窗口内经过的 CPU 周期数
 * @param cpu_freq CPU 频率 [Hz]
 * @return 平均频率 [Hz]；窗口为空时返回 0
 */
static uint32_t rate_hz(uint32_t delta, uint32_t elapsed_cycles, uint32_t cpu_freq) {
    if (elapsed_cycles == 0U || cpu_freq == 0U) {
        return 0U;
    }

    return (uint32_t)(((uint64_t)delta * cpu_freq + elapsed_cycles / 2U) / elapsed_cycles);
}

void app_debug_profiler_dump(uint32_t cpu_freq, uint32_t elapsed_cycles) {
    irq_prof_result_t result;
    uint8_t slot_count = irq_prof_get_slot_count();
    irq_prof_cycle_t overhead = irq_prof_get_overhead_cycles();

    static uint32_t s_last_hits[IRQ_PROF_MAX_SLOTS];
    static uint32_t s_last_outliers[IRQ_PROF_MAX_SLOTS];
    static bool s_initialized;

    float factor_ns = (cpu_freq > 0U) ? (1e9f / (float)cpu_freq) : 0.0f;

    app_debug_printf(
        "\r\n[IRQ_PROF] CPU=%uMHz, overhead=%u cycles, elapsed=%lu us\r\n", cpu_freq / 1000000,
        overhead, (unsigned long)((uint64_t)elapsed_cycles * 1000000ULL / cpu_freq));
    app_debug_printf(
        "----------------------------------------------------------------------------\r\n");
    app_debug_printf(
        "  Slot  Label         last    min    max    avg  ns  hits(+d,Hz)        out(+d)\r\n");
    app_debug_printf(
        "----------------------------------------------------------------------------\r\n");

    for (uint8_t i = 0; i < slot_count; i++) {
        if (irq_prof_get_result(i, &result) != 0) {
            continue;
        }

        uint32_t delta = s_initialized ? result.hits - s_last_hits[i] : 0U;
        uint32_t outlier_delta = s_initialized ? result.outliers - s_last_outliers[i] : 0U;
        uint32_t hz = rate_hz(delta, elapsed_cycles, cpu_freq);

        app_debug_printf(
            "  [%02u]  %-12s %5u %5u %5u %5u      n=%lu(+%lu,%luHz) out=%lu(+%lu)\r\n", i,
            irq_prof_get_label(i), (uint32_t)(result.last_ns * factor_ns),
            (uint32_t)(result.min_ns * factor_ns), (uint32_t)(result.max_ns * factor_ns),
            (uint32_t)(result.avg_ns * factor_ns), (unsigned long)result.hits, (unsigned long)delta,
            (unsigned long)hz, (unsigned long)result.outliers, (unsigned long)outlier_delta);

        s_last_hits[i] = result.hits;
        s_last_outliers[i] = result.outliers;
    }

    s_initialized = true;

    app_debug_printf(
        "----------------------------------------------------------------------------\r\n");
}
