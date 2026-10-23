/**
 * @file id_rl.c
 * @brief 等效 Rs/Ld 拟合与 PI 计算（仅任务上下文）
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "id_rl.h"
#include "foc_math.h"
#include <math.h>
#include <stddef.h>

/* 平滑超调用于抑制采样噪声；raw 上限防止单点尖峰被平滑掩盖。 */

/** @brief 中心化线性回归，避免大偏置的小信号消减误差 */
static int fit_line(const float* x, const float* y, uint16_t count, id_rl_fit_t* out) {
    double mx = 0, my = 0, xx = 0, xy = 0, error = 0;
    if (!x || !y || !out || count < 3)
        return -1;
    for (uint16_t i = 0; i < count; ++i) {
        if (!foc_finite(x[i]) || !foc_finite(y[i]))
            return -1;
        mx += x[i];
        my += y[i];
    }
    mx /= count;
    my /= count;
    for (uint16_t i = 0; i < count; ++i) {
        double dx = x[i] - mx;
        xx += dx * dx;
        xy += dx * (y[i] - my);
    }
    if (xx < 1e-10)
        return -1;
    out->value = (float)(xy / xx);
    out->intercept = (float)(my - out->value * mx);
    for (uint16_t i = 0; i < count; ++i) {
        double e = y[i] - out->value * x[i] - out->intercept;
        error += e * e;
    }
    out->residual = (float)sqrt(error / count);
    return foc_finite(out->value) && foc_finite(out->residual) ? 0 : -1;
}
int id_rl_resistance(const float* current, const float* voltage, uint16_t count, id_rl_fit_t* out) {
    if (fit_line(current, voltage, count, out))
        return -1;
    return out->value > 0 && out->residual <= 0.03f ? 0 : -1;
}
int id_rl_inductance(
    const id_rl_sample_t* data, uint16_t count, float dt, float resistance, id_rl_fit_t* out) {
    uint16_t end = count;
    if (!data || !out || count < 16 || count > 256 || !foc_finite(dt) || dt <= 0
        || !foc_finite(resistance) || resistance <= 0)
        return -1;
    for (uint16_t i = 0; i < count; ++i)
        if (!data[i].valid || !foc_finite(data[i].i_d_a) || !foc_finite(data[i].v_d_previous_v)
            || !foc_finite(data[i].i_q_a) || fabsf(data[i].i_q_a) > 0.2f)
            return -1;
    /* 恒压暂态直接拟合 i=A+B*exp(-t/tau)，不对带噪电流差分。
     * 排除前3拍装载边缘；若有第二平台，只使用第一段恒压数据。 */
    for (uint16_t i = 4; i < count; ++i)
        if (fabsf(data[i].v_d_previous_v - data[3].v_d_previous_v) > 0.01f) {
            end = i;
            break;
        }
    if (end < 20)
        return -1;
    double best_error = 1e30, best_tau = 0, best_amplitude = 0, best_offset = 0;
    double lower = log((double)dt), upper = log((double)dt * 100);
    for (unsigned pass = 0; pass < 3; ++pass) {
        double spacing = (upper - lower) / 24;
        for (unsigned candidate = 0; candidate <= 24; ++candidate) {
            double tau = exp(lower + candidate * spacing);
            double decay = exp(-(double)dt / tau), value = 1;
            double sx = 0, sy = 0, xx = 0, xy = 0, yy = 0;
            unsigned n = end - 3;
            for (uint16_t i = 3; i < end; ++i) {
                double y = data[i].i_d_a;
                sx += value;
                sy += y;
                xx += value * value;
                xy += value * y;
                yy += y * y;
                value *= decay;
            }
            double denominator = xx - sx * sx / n;
            if (denominator < 1e-10)
                continue;
            double amplitude = (xy - sx * sy / n) / denominator;
            double offset = (sy - amplitude * sx) / n;
            double error = fmax(0, yy - offset * sy - amplitude * xy);
            if (error < best_error) {
                best_error = error;
                best_tau = tau;
                best_amplitude = amplitude;
                best_offset = offset;
            }
        }
        if (best_tau <= 0)
            return -1;
        lower = fmax(log((double)dt), log(best_tau) - spacing);
        upper = fmin(log((double)dt * 100), log(best_tau) + spacing);
    }
    out->value = (float)(resistance * best_tau);
    out->intercept = (float)best_offset;
    out->residual = (float)sqrt(best_error / (end - 3));
    if (fabs(best_amplitude) < 0.15 || out->residual > 0.15f || best_tau <= dt * 1.01
        || best_tau >= dt * 99)
        return -1;
    return foc_finite(out->value) && out->value > 0 ? 0 : -1;
}
int id_rl_pi(float resistance, float inductance, float bandwidth_hz, float* kp, float* ki) {
    if (!kp || !ki || !foc_finite(resistance) || !foc_finite(inductance)
        || !foc_finite(bandwidth_hz) || resistance <= 0 || inductance <= 0 || bandwidth_hz <= 0)
        return -1;
    *kp = inductance * 6.283185307f * bandwidth_hz;
    *ki = resistance * 6.283185307f * bandwidth_hz;
    return foc_finite(*kp) && foc_finite(*ki) ? 0 : -1;
}
int id_rl_step(
    const id_rl_sample_t* data, uint16_t count, float dt, float initial, float target,
    id_rl_step_result_t* out) {
    if (!data || !out || count < 32 || !foc_finite(dt) || dt <= 0 || !foc_finite(initial)
        || !foc_finite(target) || target <= initial)
        return -1;
    float span = target - initial, peak_raw = initial, peak_smoothed = initial;
    float lo = target, hi = target, mean = 0, square = 0;
    double q_sum = 0;
    bool q_ok;
    int first = -1, last = -1;
    const uint16_t tail_start = (uint16_t)(count * 3U / 4U);
    const uint16_t tail_count = (uint16_t)(count - tail_start);
    for (uint16_t i = 0; i < count; ++i) {
        float value = data[i].i_d_a;
        if (!data[i].valid || !foc_finite(value) || !foc_finite(data[i].i_q_a))
            return -1;
        q_sum += data[i].i_q_a;
        if (first < 0 && value >= initial + 0.1f * span)
            first = i;
        if (last < 0 && value >= initial + 0.9f * span)
            last = i;
        if (value > peak_raw)
            peak_raw = value;
        {
            float smoothed = value;
            if (i > 0U && i + 1U < count) {
                smoothed = (data[i - 1U].i_d_a + value + data[i + 1U].i_d_a) / 3.0f;
            }
            if (smoothed > peak_smoothed)
                peak_smoothed = smoothed;
        }
        if (i >= tail_start) {
            if (i == tail_start)
                lo = hi = value;
            if (value < lo)
                lo = value;
            if (value > hi)
                hi = value;
            mean += value;
            square += value * value;
        }
    }
    mean /= (float)tail_count;
    out->rise_s = last >= first && first >= 0 ? (last - first) * dt : count * dt;
    /* 平滑后超调避免把单点采样尖峰当成动态超调；原始峰值保留诊断。 */
    out->overshoot = fmaxf(0, (peak_smoothed - target) / span);
    out->overshoot_raw = fmaxf(0, (peak_raw - target) / span);
    out->error_a = fabsf(mean - target);
    out->ripple_a = hi - lo;
    out->ripple_rms_a =
        foc_sqrtf(fmaxf(0.0f, square / (float)tail_count - mean * mean));
    /* q 轴角度/相序错误表现为整段均值偏差；单个采样尖峰不应否决。 */
    q_ok = fabsf((float)(q_sum / (double)count)) <= 0.25f;
    out->passed = q_ok && last >= 0 && out->rise_s <= 0.003f && out->overshoot <= 0.2f
               && out->overshoot_raw <= ID_RL_OVERSHOOT_RAW_MAX && out->error_a <= 0.05f
               && out->ripple_rms_a <= 0.1f;
    return 0;
}
