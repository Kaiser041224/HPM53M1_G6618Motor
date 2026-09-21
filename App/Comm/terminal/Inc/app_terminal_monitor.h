/**
 * @file    app_terminal_monitor.h
 * @brief   Terminal monitor 常驻状态区（显示期间 REPL 可正常使用）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_TERMINAL_MONITOR_H
#define APP_TERMINAL_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 1kHz 驱动（2Hz 刷新状态区；未启用时空操作）。
 * @param now_ms 系统毫秒计数
 */
void app_terminal_monitor_run_once(uint32_t now_ms);

/**
 * @brief 状态区是否启用。
 * @return true = 启用
 */
bool app_terminal_monitor_is_active(void);

/**
 * @brief 启用/停用状态区（停用时清除已绘制区域）。
 * @param on true = 启用；false = 停用
 */
void app_terminal_monitor_set(bool on);

#ifdef __cplusplus
}
#endif

#endif /* APP_TERMINAL_MONITOR_H */
