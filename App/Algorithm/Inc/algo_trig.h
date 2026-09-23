/**
 * @file    algo_trig.h
 * @brief   三角函数查表（四象限折叠 + 单表镜像，HPM SDK FOC 惯例）
 * @author  Kaiser
 *
 * 移植自 HPM SDK middleware/hpm_mcl/sensor_control/hpm_foc.c 的
 * bldc_foc_sin_cos —— HPM 电机控制库（FOC 主路径）的正统 sin/cos 做法：
 *   - 四象限折叠：角度 [0,360)° → 象限判断 → 查表 + 象限符号
 *   - 单表镜像：cos(θ) = 表[500 − i]（cos θ = sin(90°−θ)）—— 一次查表同时得 sin/cos
 *   - const 表 501 项 / 0.18° 步进，落 flash 零 RAM，无插值、无累积漂移
 *
 * 说明：HPM5361 无 TFA 硬件三角加速（TFA 仅 HPM5E00/HPM5E31/HPM6800）；
 * hpm_math 中间件要求 nds 工具链（本工程 gnu 不可用）；故采用 hpm_mcl
 * FOC 库的纯查表方案（零依赖）。FOC 阶段 park/inv-park 的 sin/cos 入参
 * 直接由本模块提供。
 *
 * Pure algorithm library — no hardware / SDK dependencies.
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ALGO_TRIG_H
#define ALGO_TRIG_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 同时求 sin/cos（一次折叠 + 镜像查表，ISR 安全：无浮点三角函数）
 * @param angle_rad 角度 [rad]，建议传入 [0, 2π)（内部安全折返）
 * @param sin_val  输出 sin(θ)（NULL 则跳过）
 * @param cos_val  输出 cos(θ)（NULL 则跳过）
 */
void algo_trig_sin_cos(float angle_rad, float* sin_val, float* cos_val);

#ifdef __cplusplus
}
#endif

#endif /* ALGO_TRIG_H */
