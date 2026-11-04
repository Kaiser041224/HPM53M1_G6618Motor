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

/** @brief step 验收的 raw 超调上限（防单点尖峰被平滑掩盖） */
#define ID_RL_OVERSHOOT_RAW_MAX (0.30f)
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
/** @brief 阶跃指标（overshoot 为 3 点平滑后；raw 仅诊断） */
typedef struct {
    float rise_s;          /**< 10%→90% 上升时间 [s] */
    float overshoot;       /**< 平滑后超调（相对阶跃幅值） */
    float overshoot_raw;   /**< 原始峰值超调（含采样噪声） */
    float error_a;         /**< 尾段平均误差 [A] */
    float ripple_a;        /**< 尾段峰峰值 [A]（含噪声，诊断用） */
    float ripple_rms_a;    /**< 尾段 RMS [A]（验收用） */
    bool passed;           /**< 超调/误差/尾段RMS验收 */
} id_rl_step_result_t;
/** @brief 多平台线性拟合电阻，返回0或-1 */
int id_rl_resistance(const float* current, const float* voltage, uint16_t count, id_rl_fit_t* out);
/** @brief 已知R的指数暂态拟合（仅诊断对照，不在主流程），返回0或-1 */
int id_rl_inductance(
    const id_rl_sample_t* data, uint16_t count, float dt, float resistance, id_rl_fit_t* out);
/** @brief 连续增益公式，带宽单位Hz，返回0或-1 */
int id_rl_pi(float resistance, float inductance, float bandwidth_hz, float* kp, float* ki);
/** @brief 正阶跃验收：平滑超调≤20%、raw≤30%、尾段误差≤0.05A、尾段RMS≤0.10A */
int id_rl_step(
    const id_rl_sample_t* data, uint16_t count, float dt, float initial, float target,
    id_rl_step_result_t* out);
/** @brief 恒压段积分辨识：0成功，-1输入，-2窗口不足，-3激励不足，-4残差超限 */
int id_rl_pulse_inductance(
    const id_rl_sample_t* data, uint16_t count, float dt, float resistance, id_rl_fit_t* out);
#endif
