/**
 * @file    algo_friction.h
 * @brief   摩擦前馈模型求值与最小二乘辨识（纯算法，零硬件依赖）
 * @author  Kaiser
 *
 * 模型（M2，轴端口径）：τ_ff(ω) = Tc·smooth_sign(ω, ω_s) + Kv·ω
 * smooth_sign：|ω|<ω_s 线性 ω/ω_s（过零连续），|ω|≥ω_s 饱和为 ±1；ω_s≤0 退化硬符号。
 * 拟合（稳态数据 τ = Tc·sign(ω) + Kv·ω）：≥4 点、双符号、全有限；普通最小二乘。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ALGO_FRICTION_H
#define ALGO_FRICTION_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 摩擦模型参数（轴端口径） */
typedef struct {
    float coulomb_nm;           /**< 库仑摩擦 [Nm] */
    float viscous_nm_per_rad_s; /**< 粘性摩擦 [Nm/(rad/s)] */
    float smoothing_rad_s;      /**< 平滑符号速度尺度 [rad/s]（模型常量，不参与拟合） */
} algo_friction_params_t;

/**
 * @brief 模型求值
 * @param model 模型参数（NULL 返回 0）
 * @param speed_rad_s 速度（非有限返回 0）
 * @return 前馈力矩 [Nm]（奇函数；符号跟随速度）
 */
float algo_friction_torque(const algo_friction_params_t* model, float speed_rad_s);

/**
 * @brief 最小二乘辨识 Tc/Kv（out->smoothing 有效则保留，否则置 0.05）
 * @param out 输出参数（coulomb/viscous 由拟合写入）
 * @param residual_rms_out 拟合残差 RMS [Nm]（可 NULL）
 * @param speeds 速度样本 [rad/s]
 * @param torques 稳态力矩样本 [Nm]（与 speeds 一一对应）
 * @param count 样本数（须 ≥ 4）
 * @return 0 = 成功；-1 = 入参/样本非法或退化（点数不足、非有限、单符号、零速度）
 */
int algo_friction_fit(
    algo_friction_params_t* out, float* residual_rms_out, const float* speeds, const float* torques,
    unsigned count);

#ifdef __cplusplus
}
#endif

#endif /* ALGO_FRICTION_H */
