/**
 * @file    app_foc_current.h
 * @brief   FOC 电流环 25kHz 执行（读输入 → Algorithm/FOC → 写逆变桥）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_FOC_CURRENT_H
#define APP_FOC_CURRENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 电流环快照（Terminal / Ozone 观测）
 */
typedef struct {
    float theta_e_rad;            /**< 本拍电角度 [rad] */
    float omega_e_rad_s;          /**< 本拍电角速度 [rad/s] */
    float i_d_a, i_q_a;           /**< d/q 反馈 [A] */
    float i_d_ref_a, i_q_ref_a;   /**< d/q 给定（限幅后）[A] */
    float v_d_v, v_q_v;           /**< d/q 电压 [V] */
    float duty_u, duty_v, duty_w; /**< 三相占空比 */
    float v_bus_v;                /**< 母线电压 [V] */
    float v_scale;                /**< 调制缩放（1.0 = 未限幅） */
    bool  saturated;              /**< 电压饱和 */
    uint32_t run_count;           /**< 执行计数 */
} app_foc_current_snapshot_t;

/**
 * @brief 初始化（构造算法对象；不使能桥）
 */
void app_foc_current_init(void);

/**
 * @brief 复位算法状态（积分器）
 */
void app_foc_current_reset(void);

/**
 * @brief 25kHz 执行：读电流/母线 → 电流环 → 调制 → 写三相占空比。
 * @param theta_e_rad 电角度 [rad]
 * @param omega_e_rad_s 电角速度 [rad/s]
 * @param i_d_ref d 轴给定 [A]
 * @param i_q_ref q 轴给定 [A]
 * @param duty_abc_out 输出三相占空比（供诊断；已写入逆变桥；可为 NULL）
 * @param saturated_out 输出电压饱和标志（可为 NULL）
 * @return 0 = 成功；-1 = 输入无效（已输出零矢量）
 */
int app_foc_current_run(float theta_e_rad, float omega_e_rad_s, float i_d_ref, float i_q_ref,
                        float duty_abc_out[3], bool* saturated_out);

/**
 * @brief 读取最近一拍快照
 */
void app_foc_current_get_snapshot(app_foc_current_snapshot_t* out);

/**
 * @brief 输出零矢量（duty = 0.5/0.5/0.5），不改变算法状态
 */
void app_foc_current_zero_vector(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_FOC_CURRENT_H */
