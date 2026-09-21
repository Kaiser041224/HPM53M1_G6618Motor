/**
 * @file    app_debug_motor.h
 * @brief   电机开环旋转自检（V/F）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_DEBUG_MOTOR_H
#define APP_DEBUG_MOTOR_H

#include <stdbool.h>
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

/**
 * @brief 旋转是否运行中（供 Shell 安全联锁使用）。
 * @return true = 运行中
 */
bool app_debug_motor_is_running(void);

/**
 * @brief 读取开环旋转状态。
 * @param freq_hz 输出电频率 [Hz]（可为 NULL）
 * @param mod 输出调制比 [0~1]（可为 NULL）
 * @param running 输出运行标志（可为 NULL）
 */
void app_debug_motor_get_state(float* freq_hz, float* mod, bool* running);

/**
 * @brief 设置电频率（限幅 0.5~10 Hz；运行中即时生效，并打印状态）。
 * @param freq_hz 电频率 [Hz]
 */
void app_debug_motor_set_freq(float freq_hz);

/**
 * @brief 设置调制比（限幅 1%~10%；运行中即时生效，并打印状态）。
 * @param mod 调制比 [0~1]（内部限幅）
 */
void app_debug_motor_set_mod(float mod);

#ifdef __cplusplus
}
#endif

#endif /* APP_DEBUG_MOTOR_H */
