/**
 * @file    algo_inertia.c
 * @brief   双向转矩阶跃惯量计算
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "algo_inertia.h"

#include "foc_math.h"

int algo_inertia_calculate(float torque_nm, float alpha_pos_rad_s2, float alpha_neg_rad_s2,
                           float rotor_inertia_kgm2, float reducer_ratio,
                           algo_inertia_result_t* out) {
    if (out == NULL) {
        return -1;
    }
    if (!foc_finite(torque_nm) || (torque_nm <= 0.0f)
        || !foc_finite(alpha_pos_rad_s2) || !foc_finite(alpha_neg_rad_s2)
        || !foc_finite(rotor_inertia_kgm2) || (rotor_inertia_kgm2 < 0.0f)
        || !foc_finite(reducer_ratio) || (reducer_ratio <= 0.0f)) {
        return -1;
    }

    float denominator = alpha_pos_rad_s2 - alpha_neg_rad_s2;
    if (!foc_finite(denominator) || (denominator <= 0.0f)) {
        return -1;
    }
    float j_total = (2.0f * torque_nm) / denominator;
    float j_rotor = rotor_inertia_kgm2 * reducer_ratio * reducer_ratio;
    if (!foc_finite(j_total) || (j_total <= 0.0f) || !foc_finite(j_rotor)) {
        return -1;
    }

    float j_load = j_total - j_rotor;
    bool clamped = false;
    if (j_load < 0.0f) {
        j_load = 0.0f;
        clamped = true;
    }
    out->j_total_kgm2 = j_total;
    out->j_rotor_reflected_kgm2 = j_rotor;
    out->j_load_kgm2 = j_load;
    out->clamped = clamped;
    return 0;
}

/** @brief 插入排序（小数组，标定期使用） */
static void inertia_sort(float* values, uint16_t count) {
    for (uint16_t i = 1U; i < count; i++) {
        float value = values[i];
        uint16_t j = i;

        while ((j > 0U) && (values[j - 1U] > value)) {
            values[j] = values[j - 1U];
            j--;
        }
        values[j] = value;
    }
}

int algo_inertia_aggregate(const float* j_total_kgm2, const bool* valid, uint16_t count,
                           float rotor_inertia_kgm2, float reducer_ratio,
                           algo_inertia_result_t* out, float* median_out, float* mad_out,
                           float* spread_out, uint16_t* valid_count_out) {
    float values[16];
    float deviations[16];
    uint16_t n = 0U;

    if ((j_total_kgm2 == NULL) || (valid == NULL) || (out == NULL) || (count == 0U)
        || (count > 16U) || !foc_finite(rotor_inertia_kgm2) || (rotor_inertia_kgm2 < 0.0f)
        || !foc_finite(reducer_ratio) || (reducer_ratio <= 0.0f)) {
        return -1;
    }
    for (uint16_t i = 0U; i < count; i++) {
        if (valid[i] && foc_finite(j_total_kgm2[i]) && (j_total_kgm2[i] > 0.0f)) {
            values[n++] = j_total_kgm2[i];
        }
    }
    if (n == 0U) {
        return -1;
    }

    inertia_sort(values, n);
    float median = (n & 1U) != 0U
                       ? values[n / 2U]
                       : 0.5f * (values[n / 2U - 1U] + values[n / 2U]);
    if (!foc_finite(median) || (median <= 0.0f)) {
        return -1;
    }
    for (uint16_t i = 0U; i < n; i++) {
        deviations[i] = fabsf(values[i] - median);
    }
    inertia_sort(deviations, n);
    float mad = (n & 1U) != 0U
                    ? deviations[n / 2U]
                    : 0.5f * (deviations[n / 2U - 1U] + deviations[n / 2U]);
    float spread = (values[n - 1U] - values[0]) / median;
    float j_rotor = rotor_inertia_kgm2 * reducer_ratio * reducer_ratio;
    float j_load = median - j_rotor;
    bool clamped = false;

    if (!foc_finite(j_rotor) || !foc_finite(j_load) || !foc_finite(spread)) {
        return -1;
    }
    if (j_load < 0.0f) {
        j_load = 0.0f;
        clamped = true;
    }
    out->j_total_kgm2 = median;
    out->j_rotor_reflected_kgm2 = j_rotor;
    out->j_load_kgm2 = j_load;
    out->clamped = clamped;
    if (median_out != NULL) {
        *median_out = median;
    }
    if (mad_out != NULL) {
        *mad_out = mad;
    }
    if (spread_out != NULL) {
        *spread_out = spread;
    }
    if (valid_count_out != NULL) {
        *valid_count_out = n;
    }
    return 0;
}
