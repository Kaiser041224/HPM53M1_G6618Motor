/**
 * @file    algo_cogging.c
 * @brief   齿槽/传动波动角表算法实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "algo_cogging.h"

#include "app_ramfunc.h" /* APP_ATTR_RAMFUNC */
#include "foc_math.h"     /* foc_wrap_2pi（ILM，替代 fmodf） */

#include <float.h>
#include <math.h>
#include <stddef.h>

#define ALGO_COGGING_TWO_PI (6.283185307179586f)

/** @brief 有限性判定（30kHz 齿槽注入路径每拍调用，须驻留 ILM） */
APP_ATTR_RAMFUNC
static bool cogging_finite(float value) { return isfinite(value); }

void algo_cogging_bin_reset(algo_cogging_bin_t* bin) {
    if (bin != NULL) {
        *bin = (algo_cogging_bin_t){0};
    }
}

void algo_cogging_bin_add(algo_cogging_bin_t* bin, float iq_a, int8_t dir) {
    if ((bin == NULL) || !cogging_finite(iq_a)) {
        return;
    }
    if (dir > 0) {
        bin->sum_fwd_a += iq_a;
        bin->count_fwd++;
    } else if (dir < 0) {
        bin->sum_rev_a += iq_a;
        bin->count_rev++;
    }
}

int algo_cogging_bin_torque(const algo_cogging_bin_t* bin, float kt_nm_per_a,
                            float reducer_ratio, bool output_side, float* torque_out) {
    if ((bin == NULL) || (torque_out == NULL) || !cogging_finite(kt_nm_per_a)
        || !cogging_finite(reducer_ratio) || (kt_nm_per_a <= 0.0f)
        || (reducer_ratio <= 0.0f) || (bin->count_fwd == 0U) || (bin->count_rev == 0U)) {
        return -1;
    }
    float avg_fwd = bin->sum_fwd_a / (float)bin->count_fwd;
    float avg_rev = bin->sum_rev_a / (float)bin->count_rev;
    float torque = (avg_fwd + avg_rev) * 0.5f * kt_nm_per_a;
    if (output_side) {
        torque *= reducer_ratio;
    }
    if (!cogging_finite(torque)) {
        return -1;
    }
    *torque_out = torque;
    return 0;
}

APP_ATTR_RAMFUNC
float algo_cogging_interp_i16(const int16_t* data, uint16_t points, float scale_nm,
                              float angle_rad) {
    if ((data == NULL) || (points < 2U) || !cogging_finite(scale_nm)
        || !cogging_finite(angle_rad)) {
        return 0.0f;
    }

    /* 正常调用只传单圈角；避免在 30kHz ISR 中调用 fmodf。 */
    float wrapped = angle_rad;
    if (wrapped < 0.0f) {
        wrapped += ALGO_COGGING_TWO_PI;
    } else if (wrapped >= ALGO_COGGING_TWO_PI) {
        wrapped -= ALGO_COGGING_TWO_PI;
    }
    if ((wrapped < 0.0f) || (wrapped >= ALGO_COGGING_TWO_PI)) {
        /* 兜底归一化不用 fmodf：libm 在 XIP，30kHz ISR 内跳回 flash 取指。
         * foc_wrap_2pi 是 ILM 纯函数，等价地把角折回 [0, 2π)。 */
        wrapped = foc_wrap_2pi(angle_rad);
        if (wrapped < 0.0f) {
            wrapped += ALGO_COGGING_TWO_PI;
        }
    }
    float position = wrapped * (float)points / ALGO_COGGING_TWO_PI;
    uint16_t index = (uint16_t)position;
    if (index >= points) {
        index = 0U;
    }
    uint16_t next = (uint16_t)((index + 1U) % points);
    float fraction = position - (float)index;
    float value = (float)data[index] + fraction * ((float)data[next] - (float)data[index]);
    return value * scale_nm;
}

int algo_cogging_finalize(const float* torque_nm, const bool* valid, uint16_t points,
                          float scale_nm, int16_t* table_out, algo_cogging_stats_t* stats_out) {
    if ((torque_nm == NULL) || (valid == NULL) || (table_out == NULL) || (points < 2U)
        || !cogging_finite(scale_nm) || (scale_nm <= 0.0f)) {
        return -1;
    }

    uint16_t valid_count = 0U;
    double sum = 0.0;
    for (uint16_t i = 0U; i < points; i++) {
        if (valid[i] && cogging_finite(torque_nm[i])) {
            sum += (double)torque_nm[i];
            valid_count++;
        }
    }
    if (valid_count < 2U) {
        return -1;
    }
    float mean = (float)(sum / (double)valid_count);

    uint16_t first_valid = points;
    for (uint16_t i = 0U; i < points; i++) {
        if (valid[i] && cogging_finite(torque_nm[i])) {
            first_valid = i;
            break;
        }
    }

    double rms_sum = 0.0;
    float minimum = FLT_MAX;
    float maximum = -FLT_MAX;
    for (uint16_t i = 0U; i < points; i++) {
        float value;
        if (valid[i] && cogging_finite(torque_nm[i])) {
            value = torque_nm[i] - mean;
        } else {
            /* 缺失箱用最近的前一个有效箱填充；质量指标仍只统计有效箱。 */
            uint16_t previous = first_valid;
            for (uint16_t k = 1U; k < points; k++) {
                uint16_t index = (uint16_t)((i + points - k) % points);
                if (valid[index] && cogging_finite(torque_nm[index])) {
                    previous = index;
                    break;
                }
            }
            value = torque_nm[previous] - mean;
        }
        float scaled = value / scale_nm;
        if (scaled > (float)INT16_MAX) {
            scaled = (float)INT16_MAX;
        } else if (scaled < (float)INT16_MIN) {
            scaled = (float)INT16_MIN;
        }
        table_out[i] = (int16_t)lrintf(scaled);

        if (valid[i] && cogging_finite(torque_nm[i])) {
            float centered = torque_nm[i] - mean;
            rms_sum += (double)centered * (double)centered;
            minimum = fminf(minimum, centered);
            maximum = fmaxf(maximum, centered);
        }
    }

    if (stats_out != NULL) {
        stats_out->rms_nm = (float)sqrt(rms_sum / (double)valid_count);
        stats_out->peak_to_peak_nm = maximum - minimum;
        stats_out->mean_nm = mean;
        stats_out->valid_bins = valid_count;
        stats_out->valid = true;
    }
    return 0;
}

int algo_cogging_finalize_bins(const algo_cogging_bin_t* bins, uint16_t points,
                               float kt_nm_per_a, float reducer_ratio, bool output_side,
                               float scale_nm, int16_t* table_out,
                               algo_cogging_stats_t* stats_out) {
    if ((bins == NULL) || (table_out == NULL) || (points < 2U)
        || !cogging_finite(scale_nm) || (scale_nm <= 0.0f)) {
        return -1;
    }

    uint16_t valid_count = 0U;
    uint16_t first_valid = points;
    double sum = 0.0;
    for (uint16_t i = 0U; i < points; i++) {
        float torque = 0.0f;
        if (algo_cogging_bin_torque(&bins[i], kt_nm_per_a, reducer_ratio, output_side,
                                    &torque)
            == 0) {
            if (first_valid == points) {
                first_valid = i;
            }
            sum += (double)torque;
            valid_count++;
        }
    }
    if ((valid_count < 2U) || (first_valid == points)) {
        return -1;
    }
    float mean = (float)(sum / (double)valid_count);

    double rms_sum = 0.0;
    float minimum = FLT_MAX;
    float maximum = -FLT_MAX;
    for (uint16_t i = 0U; i < points; i++) {
        float torque = 0.0f;
        bool valid = (algo_cogging_bin_torque(&bins[i], kt_nm_per_a, reducer_ratio,
                                              output_side, &torque)
                      == 0);
        float value;
        if (valid) {
            value = torque - mean;
        } else {
            /* 缺失箱用最近的前一个有效箱填充；质量指标只统计有效箱。 */
            uint16_t previous = first_valid;
            for (uint16_t k = 1U; k < points; k++) {
                uint16_t index = (uint16_t)((i + points - k) % points);
                float candidate = 0.0f;
                if (algo_cogging_bin_torque(&bins[index], kt_nm_per_a, reducer_ratio,
                                            output_side, &candidate)
                    == 0) {
                    previous = index;
                    break;
                }
            }
            float previous_torque = 0.0f;
            (void)algo_cogging_bin_torque(&bins[previous], kt_nm_per_a, reducer_ratio,
                                          output_side, &previous_torque);
            value = previous_torque - mean;
        }
        float scaled = value / scale_nm;
        if (scaled > (float)INT16_MAX) {
            scaled = (float)INT16_MAX;
        } else if (scaled < (float)INT16_MIN) {
            scaled = (float)INT16_MIN;
        }
        table_out[i] = (int16_t)lrintf(scaled);

        if (valid) {
            float centered = torque - mean;
            rms_sum += (double)centered * (double)centered;
            minimum = fminf(minimum, centered);
            maximum = fmaxf(maximum, centered);
        }
    }

    if (stats_out != NULL) {
        stats_out->rms_nm = (float)sqrt(rms_sum / (double)valid_count);
        stats_out->peak_to_peak_nm = maximum - minimum;
        stats_out->mean_nm = mean;
        stats_out->valid_bins = valid_count;
        stats_out->valid = true;
    }
    return 0;
}
