/**
 * @file    app_debug_profiler.h
 * @brief   CPU 周期占用分析输出
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_DEBUG_PROFILER_H
#define APP_DEBUG_PROFILER_H

#include <stdint.h>

/**
 * @brief  打印 CPU 周期占用统计。
 * @param  cpu_freq        CPU 频率 [Hz]
 * @param  elapsed_cycles  统计窗口内经过的周期数
 */
void app_debug_profiler_dump(uint32_t cpu_freq, uint32_t elapsed_cycles);

#endif /* APP_DEBUG_PROFILER_H */
