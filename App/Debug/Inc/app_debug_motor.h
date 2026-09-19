/*
 * Debug Motor - 电机开环旋转自检（V/F）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_DEBUG_MOTOR_H
#define APP_DEBUG_MOTOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化开环旋转自检（不启动旋转）。
 */
void app_debug_motor_init(void);

/**
 * @brief 主循环调用（25kHz 节拍）：旋转角度积分 + 三相占空比更新。
 */
void app_debug_motor_run_once(void);

/**
 * @brief 旋转启停切换（启动时自动使能逆变桥；停止时关断输出与 12V）。
 */
void app_debug_motor_rotation_toggle(void);

/**
 * @brief 电频率步进（±0.5Hz，限幅 0.5~10Hz）。
 * @param dir +1 增加，-1 减少
 */
void app_debug_motor_freq_step(int8_t dir);

/**
 * @brief 调制比步进（±1%，限幅 1%~10%）。
 * @param dir +1 增加，-1 减少
 */
void app_debug_motor_mod_step(int8_t dir);

/**
 * @brief 停止旋转（关输出 + 关 12V）。
 */
void app_debug_motor_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_DEBUG_MOTOR_H */
