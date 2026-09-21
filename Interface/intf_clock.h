/**
 * @file    intf_clock.h
 * @brief   时钟与延时抽象接口
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef INTF_CLOCK_H
#define INTF_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化系统时钟
 */
void intf_clock_init(void);

/**
 * @brief 读取 CPU 时钟频率
 * @return 频率 [Hz]
 */
uint32_t intf_clock_get_cpu_freq(void);

/**
 * @brief 读取 AHB 总线频率
 * @return 频率 [Hz]
 */
uint32_t intf_clock_get_ahb_freq(void);

/**
 * @brief 读取 MOT0 时钟频率（PWM0/PWM1 时钟源）
 * @return 频率 [Hz]
 */
uint32_t intf_clock_get_mot0_freq(void);

/**
 * @brief 读取当前周期计数
 * @return 周期计数值
 */
uint32_t intf_clock_get_cycle(void);

/**
 * @brief 毫秒级忙等延时
 * @param ms 延时 [ms]
 */
void intf_clock_delay_ms(uint32_t ms);

/**
 * @brief 微秒级忙等延时
 * @param us 延时 [us]
 */
void intf_clock_delay_us(uint32_t us);

#ifdef __cplusplus
}
#endif

#endif /* INTF_CLOCK_H */
