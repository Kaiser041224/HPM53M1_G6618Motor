/**
 * @file id_rl.h
 * @brief 等效 Rs/Ld 辨识及电流 PI 验收契约
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef ID_RL_H
#define ID_RL_H
#include <stdbool.h>
#include <stdint.h>
/** @brief 同步样本，电压是产生本拍电流的前一拍指令 */
typedef struct {
    float i_d_a, i_q_a, v_d_previous_v;
    float i_u_a, i_v_a, i_w_a; /**< 实测三相，不通过KCL重建，供诊断 */
    bool valid;
} id_rl_sample_t;
/** @brief 拟合结果 */
typedef struct {
    float value, intercept, residual;
} id_rl_fit_t;
/** @brief 阶跃指标 */
typedef struct {
    float rise_s, overshoot, error_a, ripple_a;
    bool passed;
} id_rl_step_result_t;
/** @brief 多平台线性拟合电阻，返回0或-1 */
int id_rl_resistance(const float* current, const float* voltage, uint16_t count, id_rl_fit_t* out);
/** @brief 已知R的指数暂态拟合（仅诊断对照，不在主流程），返回0或-1 */
int id_rl_inductance(
    const id_rl_sample_t* data, uint16_t count, float dt, float resistance, id_rl_fit_t* out);
/** @brief 连续增益公式，带宽单位Hz，返回0或-1 */
int id_rl_pi(float resistance, float inductance, float bandwidth_hz, float* kp, float* ki);
/** @brief 正阶跃验收：超调≤20%、尾段误差≤0.05A、峰峰纹波≤0.10A */
int id_rl_step(
    const id_rl_sample_t* data, uint16_t count, float dt, float initial, float target,
    id_rl_step_result_t* out);
/** @brief 恒压段积分辨识：0成功，-1输入，-2窗口不足，-3激励不足，-4残差超限 */
int id_rl_pulse_inductance(
    const id_rl_sample_t* data, uint16_t count, float dt, float resistance, id_rl_fit_t* out);
#endif
