/**
 * @file    foc_modulation.c
 * @brief   min-max 零序注入调制实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "foc_modulation.h"

/**
 * @brief 三相最小值
 */
FOC_ATTR_RAMFUNC
static inline float foc_min3(float a, float b, float c) {
    float m = (a < b) ? a : b;
    return (m < c) ? m : c;
}

/**
 * @brief 三相最大值
 */
FOC_ATTR_RAMFUNC
static inline float foc_max3(float a, float b, float c) {
    float m = (a > b) ? a : b;
    return (m > c) ? m : c;
}

FOC_ATTR_RAMFUNC
int foc_modulation_step(const foc_modulation_cfg_t* cfg, float v_alpha, float v_beta, float v_bus_v,
                        float duty_abc[3], float* v_scale_out) {
    float vu, vv, vw, vmax, vmin, span, span_max, scale = 1.0f, offset, inv_vbus;

    if ((cfg == NULL) || (duty_abc == NULL)) {
        return -1;
    }
    if (!foc_finite(v_alpha) || !foc_finite(v_beta) || !foc_finite(v_bus_v)) {
        return -1;
    }
    if (!foc_finite(cfg->duty_max) || (cfg->duty_max <= 0.5f) || (cfg->duty_max > 1.0f)) {
        return -1;
    }
    if (!foc_finite(cfg->v_bus_min) || (cfg->v_bus_min <= 0.0f)) {
        return -1; /* 同时保证 v_bus_v ≥ v_bus_min > 0（除法安全） */
    }
    if (v_bus_v < cfg->v_bus_min) {
        return -1;
    }

    foc_inv_clarke(v_alpha, v_beta, &vu, &vv, &vw);

    vmax = foc_max3(vu, vv, vw);
    vmin = foc_min3(vu, vv, vw);
    span = vmax - vmin;
    span_max = (2.0f * cfg->duty_max - 1.0f) * v_bus_v;
    if ((span > span_max) && (span > 0.0f)) {
        scale = span_max / span;
        vu *= scale;
        vv *= scale;
        vw *= scale;
        vmax = foc_max3(vu, vv, vw);
        vmin = foc_min3(vu, vv, vw);
    }

    offset = -0.5f * (vmax + vmin);
    inv_vbus = 1.0f / v_bus_v; /* 单次除法（v_bus_v ≥ v_bus_min > 0 已保证） */
    duty_abc[0] = 0.5f + (vu + offset) * inv_vbus;
    duty_abc[1] = 0.5f + (vv + offset) * inv_vbus;
    duty_abc[2] = 0.5f + (vw + offset) * inv_vbus;

    for (uint8_t i = 0U; i < 3U; i++) {
        if (duty_abc[i] > cfg->duty_max) {
            duty_abc[i] = cfg->duty_max;
        } else if (duty_abc[i] < (1.0f - cfg->duty_max)) {
            duty_abc[i] = 1.0f - cfg->duty_max;
        }
    }

    if (v_scale_out != NULL) {
        *v_scale_out = scale;
    }
    return 0;
}
