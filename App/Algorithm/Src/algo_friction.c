/**
 * @file    algo_friction.c
 * @brief   摩擦前馈模型求值与最小二乘辨识实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "algo_friction.h"
#include "foc_math.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

/** @brief 平滑符号：过零线性、外侧饱和；ω_s≤0 退化硬符号（motion 10kHz 热路径，入 ILM） */
FOC_ATTR_RAMFUNC
static float friction_smooth_sign(float speed, float smoothing) {
    if (!(smoothing > 0))
        return speed > 0 ? 1.0f : -1.0f;
    float scaled = speed / smoothing;
    return scaled > 1.0f ? 1.0f : scaled < -1.0f ? -1.0f : scaled;
}

FOC_ATTR_RAMFUNC
float algo_friction_torque(const algo_friction_params_t* model, float speed_rad_s) {
    if (model == NULL || !isfinite(speed_rad_s))
        return 0.0f;
    return model->coulomb_nm * friction_smooth_sign(speed_rad_s, model->smoothing_rad_s)
         + model->viscous_nm_per_rad_s * speed_rad_s;
}

int algo_friction_fit(
    algo_friction_params_t* out, float* residual_rms_out, const float* speeds, const float* torques,
    unsigned count) {
    if (out == NULL || speeds == NULL || torques == NULL || count < 4)
        return -1;
    double s11 = 0, s12 = 0, s22 = 0, b1 = 0, b2 = 0;
    bool have_positive = false, have_negative = false;

    for (unsigned i = 0; i < count; ++i) {
        if (!isfinite(speeds[i]) || !isfinite(torques[i]))
            return -1;
        if (speeds[i] > 0)
            have_positive = true;
        else if (speeds[i] < 0)
            have_negative = true;
        double sign = speeds[i] > 0 ? 1.0 : speeds[i] < 0 ? -1.0 : 0.0;
        double w = speeds[i];
        s11 += sign * sign;
        s12 += sign * w;
        s22 += w * w;
        b1 += torques[i] * sign;
        b2 += torques[i] * w;
    }
    /* 双符号才可分离 Tc/Kv；退化基（det≈0）拒绝 */
    if (!have_positive || !have_negative)
        return -1;
    double det = s11 * s22 - s12 * s12;
    if (!(det > 1e-12))
        return -1;
    double coulomb = (b1 * s22 - b2 * s12) / det;
    double viscous = (s11 * b2 - s12 * b1) / det;
    double sum_sq = 0;
    for (unsigned i = 0; i < count; ++i) {
        double error = torques[i]
                     - coulomb
                           * (speeds[i] > 0   ? 1.0
                              : speeds[i] < 0 ? -1.0
                                              : 0.0)
                     - viscous * speeds[i];
        sum_sq += error * error;
    }
    out->coulomb_nm = (float)coulomb;
    out->viscous_nm_per_rad_s = (float)viscous;
    if (!(out->smoothing_rad_s > 0))
        out->smoothing_rad_s = 0.05f;
    if (residual_rms_out != NULL)
        *residual_rms_out = (float)sqrt(sum_sq / count);
    return 0;
}
