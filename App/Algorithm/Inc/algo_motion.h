/** @file algo_motion.h
 * @brief 游标解算与速度PI（圈输入、轴端rad输出）
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef ALGO_MOTION_H
#define ALGO_MOTION_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    float integral;
    bool saturated; /**< 外部执行限幅（FOC限流/电压饱和/请求过期）；饱和时仅误差反向允许积分 */
} algo_motion_pi_t;
/** @brief 游标多圈解算；rotor/vernier在[0,1)，reducer/output_ratio 来自 motor.yaml */
int algo_motion_vernier(float rotor, float vernier, float phase, float reducer_ratio,
                        float output_ratio, float* shaft_rad);
/** @brief ODrive POS_FILTER 二阶临界阻尼滤波状态（pos/vel 设定，ZOH 输入间平滑） */
typedef struct {
    float pos_setpoint_rad;   /**< 滤波后位置设定 [rad] */
    float vel_setpoint_rad_s; /**< 滤波后速度设定 [rad/s]（含 input_vel 跟踪） */
} algo_motion_pos_filter_t;
/** @brief ODrive POS_FILTER 单步：bw钳位≤0.25/dt；bw≤0直通；无效输入不更新 */
void algo_motion_pos_filter_step(
    algo_motion_pos_filter_t* f, float input_pos, float input_vel, float bandwidth_rad_s, float dt);
/** @brief 位置P+速度前馈：vel = vel_ff + pos_gain*(pos_sp - pos_est) */
float algo_motion_position(float pos_setpoint, float pos_est, float pos_gain, float vel_ff);
/** @brief 速度PI条件积分，输出轴端Nm，ki为连续增益 */
float algo_motion_speed(
    algo_motion_pi_t* pi, float target, float speed, float kp, float ki, float dt, float limit);
#ifdef __cplusplus
}
#endif
#endif
