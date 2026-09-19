/*
 * Debug RTT - SEGGER RTT wrapper for debug output
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_DEBUG_RTT_H
#define APP_DEBUG_RTT_H

#include <stdint.h>

/* ============================================================================
 * 周期调试打印总开关（2026-09-19）
 *   1 = 正常：心跳 / 编码器统计 / CAN 周期行全部输出
 *   0 = 静默：仅保留启动自检与按需命令（d/p/k/n）输出，便于专注观测 ADC
 * 用法：在 CMakeLists.txt 或编译命令行定义 -DAPP_DEBUG_PERIODIC_PRINT=1 打开
 * ============================================================================ */
#ifndef APP_DEBUG_PERIODIC_PRINT
#define APP_DEBUG_PERIODIC_PRINT (0)
#endif

int app_debug_printf(const char *fmt, ...);

/**
 * @brief 获取 RTT 写入成功次数。
 *
 * @return 已成功写入 RTT 上行缓冲的字符串条数。
 */
uint32_t app_debug_rtt_get_write_ok(void);

/**
 * @brief 获取 RTT 写入被丢弃次数。
 *
 * @return 因 RTT 上行缓冲不足被整条丢弃的字符串条数。
 */
uint32_t app_debug_rtt_get_write_dropped(void);

#endif /* APP_DEBUG_RTT_H */
