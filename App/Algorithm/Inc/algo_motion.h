/** @file algo_motion.h
 * @brief 游标解算与速度PI（圈输入、轴端rad输出）
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef ALGO_MOTION_H
#define ALGO_MOTION_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    float integral;
} algo_motion_pi_t;
/** @brief 49/50游标；rotor/vernier在[0,1)，phase在[0,1/50)，残差>0.25圈拒绝 */
int algo_motion_vernier(float rotor, float vernier, float phase, float* shaft_rad);
/** @brief 速度PI条件积分，输出轴端Nm，ki为连续增益 */
float algo_motion_speed(
    algo_motion_pi_t* pi, float target, float speed, float kp, float ki, float dt, float limit);
#ifdef __cplusplus
}
#endif
#endif
