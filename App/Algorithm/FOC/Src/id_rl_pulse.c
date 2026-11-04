/**
 * @file id_rl_pulse.c
 * @brief 交替电压恒压段积分辨识（任务上下文）
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "foc_math.h"
#include "id_rl.h"
#include <math.h>
#include <stddef.h>

int id_rl_pulse_inductance(
    const id_rl_sample_t* data, uint16_t count, float dt, float resistance, id_rl_fit_t* out) {
    if (!data || !out || count < 32 || count > 256 || !foc_finite(dt) || dt <= 0
        || !foc_finite(resistance) || resistance <= 0)
        return -1;
    for (unsigned i = 0; i < count; ++i)
        if (!data[i].valid || !foc_finite(data[i].i_d_a) || !foc_finite(data[i].v_d_previous_v))
            return -1;
    double x[64], y[64], duration[64];
    unsigned used = 0, start = 0;
    /* 两个相邻端点均值具有相同时间中心，不将端点滤波延迟算入L。
     * y使用与两组端点完全对应的两个梯形积分的均值。 */
    while (start < count) {
        unsigned end = start + 1;
        while (end < count && fabsf(data[end].v_d_previous_v - data[start].v_d_previous_v) < 0.02f)
            ++end;
        if (end - start >= 4 && start > 0 && end < count && used < 64) {
            unsigned a = start + 1, b = end - 2;
            double delta =
                0.5 * (data[b].i_d_a + data[b + 1].i_d_a - data[a].i_d_a - data[a + 1].i_d_a);
            double integral = 0;
            for (unsigned j = a + 1; j <= b + 1; ++j) {
                double weight = (j == a + 1 || j == b + 1) ? 0.5 : 1.0;
                integral += weight * dt
                          * (data[j].v_d_previous_v
                             - resistance * 0.5 * (data[j].i_d_a + data[j - 1].i_d_a));
            }
            duration[used] = (b - a) * dt;
            x[used] = delta / duration[used];
            y[used] = integral / duration[used];
            ++used;
        }
        start = end;
    }
    if (used < 6)
        return -2;
    double mx = 0, my = 0, xx = 0, xy = 0;
    for (unsigned i = 0; i < used; ++i) {
        mx += x[i];
        my += y[i];
    }
    mx /= used;
    my /= used;
    for (unsigned i = 0; i < used; ++i) {
        xx += (x[i] - mx) * (x[i] - mx);
        xy += (x[i] - mx) * (y[i] - my);
    }
    if (xx / used < 100)
        return -3;
    out->value = (float)(xy / xx);
    out->intercept = (float)(my - out->value * mx);
    if (!foc_finite(out->value) || out->value <= 0)
        return -3;
    double error = 0;
    for (unsigned i = 0; i < used; ++i) {
        double residual = (y[i] - out->intercept - out->value * x[i]) * duration[i] / out->value;
        error += residual * residual;
    }
    out->residual = (float)sqrt(error / used); /* 段电流变化残差，不是逐点差分残差 */
    return out->residual <= 0.15f ? 0 : -4;
}
