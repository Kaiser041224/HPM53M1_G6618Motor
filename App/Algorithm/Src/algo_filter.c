/**
 * @file    algo_filter.c
 * @brief   滤波库（滑动平均 / 一阶低通 / FIR / Biquad / 中值）实现
 * @author  Kaiser
 *
 * Filter Library Implementation
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "algo_filter.h"

#include <stddef.h>

/* ILM deployment for hot ISR paths */
#ifndef ALGO_FILTER_RAMFUNC
# define ALGO_FILTER_RAMFUNC __attribute__((section(".fast")))
#endif

/* ═══════════════════════════════════════════════════════════════════════
 *  Moving Average
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief 初始化滑动平均对象
 * @param self 对象
 * @param cfg 配置
 * @return 0 成功；负数错误码
 */
static int algo_ma_init_impl(algo_ma_t* self, const algo_ma_cfg_t* cfg) {
    if (self == NULL)
        return -1;
    if (cfg == NULL)
        return -2;
    if (cfg->buffer == NULL)
        return -3;
    if (cfg->window_size == 0)
        return -4;

    self->_buf = cfg->buffer;
    self->_size = cfg->window_size;
    self->_inv_size = 1.0f / (float)cfg->window_size;
    self->_idx = 0;
    self->_sum = 0.0f;
    self->_filled = false;
    self->_inited = true;

    for (uint16_t i = 0; i < self->_size; i++) {
        self->_buf[i] = 0.0f;
    }

    return 0;
}

/**
 * @brief 滑动平均单步计算（委托内联快速路径）
 * @param self 对象
 * @param x 输入样本
 * @return 滤波输出
 */
ALGO_FILTER_RAMFUNC
static float algo_ma_step_impl(algo_ma_t* self, float x) { return algo_ma_step_fast(self, x); }

/**
 * @brief 滑动平均复位
 * @param self 对象
 */
static void algo_ma_reset_impl(algo_ma_t* self) {
    if (!self->_inited)
        return;

    self->_idx = 0;
    self->_sum = 0.0f;
    self->_filled = false;

    for (uint16_t i = 0; i < self->_size; i++) {
        self->_buf[i] = 0.0f;
    }
}

/**
 * @brief 构造滑动平均对象
 * @param self 对象
 */
void algo_ma_ctor(algo_ma_t* self) {
    if (self == NULL)
        return;

    self->init = algo_ma_init_impl;
    self->step = algo_ma_step_impl;
    self->reset = algo_ma_reset_impl;
    self->_buf = NULL;
    self->_size = 0;
    self->_inv_size = 0.0f;
    self->_idx = 0;
    self->_sum = 0.0f;
    self->_filled = false;
    self->_inited = false;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  1st-Order Low-Pass
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief 初始化一阶低通对象
 * @param self 对象
 * @param cfg 配置
 * @return 0 成功；负数错误码
 */
static int algo_lpf_init_impl(algo_lpf_t* self, const algo_lpf_cfg_t* cfg) {
    if (self == NULL)
        return -1;
    if (cfg == NULL)
        return -2;

    float sample_rate_hz = cfg->sample_rate_hz;
    float cutoff_hz = cfg->cutoff_hz;

    if (cutoff_hz <= 0.0f)
        return -3;
    if (sample_rate_hz <= 0.0f)
        return -4;
    if (cutoff_hz >= sample_rate_hz * 0.5f)
        return -5;
    if (!algo_flt_finite(cutoff_hz))
        return -6;
    if (!algo_flt_finite(sample_rate_hz))
        return -7;

    float sample_time_s = 1.0f / sample_rate_hz;
    float time_const_s = 1.0f / (2.0f * ALGO_PI_F * cutoff_hz);
    self->_alpha = sample_time_s / (sample_time_s + time_const_s);
    self->_y = 0.0f;
    self->_primed = false;
    self->_inited = true;

    return 0;
}

/**
 * @brief 一阶低通单步计算（委托内联快速路径）
 * @param self 对象
 * @param x 输入样本
 * @return 滤波输出
 */
static float algo_lpf_step_impl(algo_lpf_t* self, float x) { return algo_lpf_step_fast(self, x); }

/**
 * @brief 一阶低通复位
 * @param self 对象
 */
static void algo_lpf_reset_impl(algo_lpf_t* self) {
    if (!self->_inited)
        return;

    self->_y = 0.0f;
    self->_primed = false;
}

/**
 * @brief 构造一阶低通对象
 * @param self 对象
 */
void algo_lpf_ctor(algo_lpf_t* self) {
    if (self == NULL)
        return;

    self->init = algo_lpf_init_impl;
    self->step = algo_lpf_step_impl;
    self->reset = algo_lpf_reset_impl;
    self->_alpha = 0.0f;
    self->_y = 0.0f;
    self->_primed = false;
    self->_inited = false;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  FIR
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief 初始化 FIR 对象
 * @param self 对象
 * @param cfg 配置
 * @return 0 成功；负数错误码
 */
static int algo_fir_init_impl(algo_fir_t* self, const algo_fir_cfg_t* cfg) {
    if (self == NULL)
        return -1;
    if (cfg == NULL)
        return -2;
    if (cfg->coeffs == NULL)
        return -3;
    if (cfg->buffer == NULL)
        return -4;
    if (cfg->num_taps == 0)
        return -5;

    for (uint16_t i = 0; i < cfg->num_taps; i++) {
        if (!algo_flt_finite(cfg->coeffs[i]))
            return -6;
    }

    self->_coeffs = cfg->coeffs;
    self->_buf = cfg->buffer;
    self->_taps = cfg->num_taps;
    self->_idx = 0;
    self->_y = 0.0f;
    self->_inited = true;

    for (uint16_t i = 0; i < self->_taps; i++) {
        self->_buf[i] = 0.0f;
    }

    return 0;
}

/**
 * @brief FIR 单步计算
 * @param self 对象
 * @param x 输入样本
 * @return 滤波输出
 */
static float algo_fir_step_impl(algo_fir_t* self, float x) {
    if (!self->_inited)
        return 0.0f;

    if (!algo_flt_finite(x))
        return self->_y;

    self->_buf[self->_idx] = x;

    float sum = 0.0f;
    uint16_t index = self->_idx;

    for (uint16_t k = 0; k < self->_taps; k++) {
        sum += self->_coeffs[k] * self->_buf[index];
        if (index == 0) {
            index = self->_taps - 1U;
        } else {
            index--;
        }
    }

    self->_idx++;
    if (self->_idx >= self->_taps)
        self->_idx = 0;

    self->_y = sum;
    return sum;
}

/**
 * @brief FIR 复位
 * @param self 对象
 */
static void algo_fir_reset_impl(algo_fir_t* self) {
    if (!self->_inited)
        return;

    self->_idx = 0;
    self->_y = 0.0f;

    for (uint16_t i = 0; i < self->_taps; i++) {
        self->_buf[i] = 0.0f;
    }
}

/**
 * @brief 构造 FIR 对象
 * @param self 对象
 */
void algo_fir_ctor(algo_fir_t* self) {
    if (self == NULL)
        return;

    self->init = algo_fir_init_impl;
    self->step = algo_fir_step_impl;
    self->reset = algo_fir_reset_impl;
    self->_coeffs = NULL;
    self->_buf = NULL;
    self->_taps = 0;
    self->_idx = 0;
    self->_y = 0.0f;
    self->_inited = false;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Biquad  (Direct Form II Transposed, a0 = 1)
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief 初始化 Biquad 对象
 * @param self 对象
 * @param cfg 配置
 * @return 0 成功；负数错误码
 */
static int algo_biquad_init_impl(algo_biquad_t* self, const algo_biquad_cfg_t* cfg) {
    if (self == NULL)
        return -1;
    if (cfg == NULL)
        return -2;

    float b0 = cfg->coeffs.b0, b1 = cfg->coeffs.b1, b2 = cfg->coeffs.b2;
    float a1 = cfg->coeffs.a1, a2 = cfg->coeffs.a2;

    if (!algo_flt_finite(b0))
        return -3;
    if (!algo_flt_finite(b1))
        return -4;
    if (!algo_flt_finite(b2))
        return -5;
    if (!algo_flt_finite(a1))
        return -6;
    if (!algo_flt_finite(a2))
        return -7;

    self->_b0 = b0;
    self->_b1 = b1;
    self->_b2 = b2;
    self->_a1 = a1;
    self->_a2 = a2;
    self->_z1 = 0.0f;
    self->_z2 = 0.0f;
    self->_y = 0.0f;
    self->_inited = true;

    return 0;
}

/**
 * @brief Biquad 单步计算（委托内联快速路径）
 * @param self 对象
 * @param x 输入样本
 * @return 滤波输出
 */
static float algo_biquad_step_impl(algo_biquad_t* self, float x) {
    return algo_biquad_step_fast(self, x);
}

/**
 * @brief Biquad 复位
 * @param self 对象
 */
static void algo_biquad_reset_impl(algo_biquad_t* self) {
    if (!self->_inited)
        return;

    self->_z1 = 0.0f;
    self->_z2 = 0.0f;
    self->_y = 0.0f;
}

/**
 * @brief 构造 Biquad 对象
 * @param self 对象
 */
void algo_biquad_ctor(algo_biquad_t* self) {
    if (self == NULL)
        return;

    self->init = algo_biquad_init_impl;
    self->step = algo_biquad_step_impl;
    self->reset = algo_biquad_reset_impl;
    self->_b0 = 0.0f;
    self->_b1 = 0.0f;
    self->_b2 = 0.0f;
    self->_a1 = 0.0f;
    self->_a2 = 0.0f;
    self->_z1 = 0.0f;
    self->_z2 = 0.0f;
    self->_y = 0.0f;
    self->_inited = false;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Median
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief 初始化中值滤波对象
 * @param self 对象
 * @param cfg 配置
 * @return 0 成功；负数错误码
 */
static int algo_med_init_impl(algo_med_t* self, const algo_med_cfg_t* cfg) {
    if (self != NULL)
        self->_inited = false;

    if (self == NULL)
        return -1;
    if (cfg == NULL)
        return -2;
    if (cfg->buffer == NULL)
        return -3;
    if (cfg->sort_buf == NULL)
        return -4;
    if (cfg->window_size == 0)
        return -5;
    if (cfg->window_size < 3)
        return -6;

    self->_buf = cfg->buffer;
    self->_sort = cfg->sort_buf;
    self->_size = cfg->window_size;
    self->_idx = 0;
    self->_count = 0;
    self->_y = 0.0f;
    self->_inited = true;

    for (uint16_t i = 0; i < self->_size; i++) {
        self->_buf[i] = 0.0f;
        self->_sort[i] = 0.0f;
    }

    return 0;
}

/**
 * @brief 中值单步计算（插入排序求中值）
 * @param self 对象
 * @param x 输入样本
 * @return 滤波输出
 */
static float algo_med_step_impl(algo_med_t* self, float x) {
    if (self == NULL || !self->_inited)
        return 0.0f;

    if (!algo_flt_finite(x))
        return self->_y;

    self->_buf[self->_idx] = x;

    self->_idx++;
    if (self->_idx >= self->_size)
        self->_idx = 0;

    if (self->_count < self->_size)
        self->_count++;

    uint16_t count = self->_count;

    for (uint16_t i = 0; i < count; i++) {
        self->_sort[i] = self->_buf[i];
    }

    for (uint16_t i = 1; i < count; i++) {
        float key = self->_sort[i];
        uint16_t j = i;
        while (j > 0 && self->_sort[j - 1] > key) {
            self->_sort[j] = self->_sort[j - 1];
            j--;
        }
        self->_sort[j] = key;
    }

    if (count & 1U) {
        self->_y = self->_sort[count / 2U];
    } else {
        self->_y = 0.5f * (self->_sort[count / 2U] + self->_sort[count / 2U - 1U]);
    }

    return self->_y;
}

/**
 * @brief 中值滤波复位
 * @param self 对象
 */
static void algo_med_reset_impl(algo_med_t* self) {
    if (self == NULL || !self->_inited)
        return;

    self->_idx = 0;
    self->_count = 0;
    self->_y = 0.0f;

    for (uint16_t i = 0; i < self->_size; i++) {
        self->_buf[i] = 0.0f;
        self->_sort[i] = 0.0f;
    }
}

/**
 * @brief 构造中值滤波对象
 * @param self 对象
 */
void algo_med_ctor(algo_med_t* self) {
    if (self == NULL)
        return;

    self->init = algo_med_init_impl;
    self->step = algo_med_step_impl;
    self->reset = algo_med_reset_impl;
    self->_buf = NULL;
    self->_sort = NULL;
    self->_size = 0;
    self->_idx = 0;
    self->_count = 0;
    self->_y = 0.0f;
    self->_inited = false;
}
