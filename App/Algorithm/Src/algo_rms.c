/**
 * @file    algo_rms.c
 * @brief   真有效值（滑动窗 True RMS / AC RMS）实现
 * @author  Kaiser
 *
 * RMS Implementation
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "algo_rms.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

static int algo_rms_init_impl(algo_rms_t* self, const algo_rms_cfg_t* cfg);
static float algo_rms_step_impl(algo_rms_t* self, float x);
static void algo_rms_reset_impl(algo_rms_t* self);
static float algo_rms_get_rms_impl(const algo_rms_t* self);
static float algo_rms_get_mean_sq_impl(const algo_rms_t* self);
static float algo_rms_update_sq_impl(algo_rms_t* self, float x);
static void algo_rms_rebuild_sum_sq(algo_rms_t* self);
static float algo_rms_calc_mean_sq(const algo_rms_t* self);

/**
 * @brief 计算当前窗口均方值
 * @param self 对象
 * @return 均方值；未初始化时返回 0
 */
static float algo_rms_calc_mean_sq(const algo_rms_t* self) {
    if (self == NULL || !self->_inited)
        return 0.0f;

    float denom =
        self->_filled ? (float)self->_size : (float)((self->_count > 0U) ? self->_count : 1U);
    float mean_sq = self->_sum_sq / denom;

    if (self->_remove_dc) {
        float mean = self->_sum / denom;
        mean_sq -= mean * mean;
        if (mean_sq < 0.0f)
            mean_sq = 0.0f;
    }

    return mean_sq;
}

/* ── Init ─────────────────────────────────────────────────────────────── */

/**
 * @brief 初始化 RMS 对象
 * @param self 对象
 * @param cfg 配置
 * @return 0 成功；负数错误码
 */
static int algo_rms_init_impl(algo_rms_t* self, const algo_rms_cfg_t* cfg) {
    if (self != NULL)
        self->_inited = false;

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
    self->_count = 0;
    self->_sum_sq = 0.0f;
    self->_sum = 0.0f;
    self->_y = 0.0f;
    self->_remove_dc = cfg->remove_dc;
    self->_filled = false;
    self->_inited = true;

    for (uint16_t i = 0; i < self->_size; i++) {
        self->_buf[i] = 0.0f;
    }

    return 0;
}

/* ── Rebuild _sum_sq / _sum from buffer (robustness) ─────────────────── */

/**
 * @brief 由缓冲重建线性和与平方和（数值异常时兜底）
 * @param self 对象
 */
static void algo_rms_rebuild_sum_sq(algo_rms_t* self) {
    self->_sum_sq = 0.0f;
    self->_sum = 0.0f;

    uint16_t count = self->_filled ? self->_size : self->_count;

    for (uint16_t i = 0; i < count; i++) {
        float value = self->_buf[i];
        if (!algo_rms_finite(value)) {
            self->_buf[i] = 0.0f;
            value = 0.0f;
        }
        float value_sq = value * value;
        self->_sum_sq += value_sq;
        self->_sum += value;
    }

    if (!algo_rms_finite(self->_sum_sq))
        self->_sum_sq = 0.0f;
    if (!algo_rms_finite(self->_sum))
        self->_sum = 0.0f;
}

/* ── update_sq: update buffer & _sum_sq, return mean_sq, no sqrtf ────── */

/**
 * @brief 更新窗口并返回均方值（不取平方根）
 * @param self 对象
 * @param x 输入样本
 * @return 均方值
 */
static float algo_rms_update_sq_impl(algo_rms_t* self, float x) {
    if (self == NULL || !self->_inited)
        return 0.0f;

    if (!algo_rms_finite(x))
        return algo_rms_calc_mean_sq(self);

    float x_sq = x * x;
    if (!algo_rms_finite(x_sq))
        return algo_rms_calc_mean_sq(self);

    float old = self->_buf[self->_idx];
    self->_buf[self->_idx] = x;

    self->_idx++;
    if (self->_idx >= self->_size)
        self->_idx = 0;

    if (self->_count < self->_size)
        self->_count++;

    if (self->_filled) {
        float old_sq = old * old;
        if (!algo_rms_finite(old_sq))
            old_sq = 0.0f;
        self->_sum_sq += x_sq - old_sq;
        self->_sum += x - old;
    } else {
        self->_sum_sq += x_sq;
        self->_sum += x;
        if (self->_count >= self->_size)
            self->_filled = true;
    }

    if (!algo_rms_finite(self->_sum_sq))
        algo_rms_rebuild_sum_sq(self);
    if (self->_sum_sq < 0.0f)
        self->_sum_sq = 0.0f;

    return algo_rms_calc_mean_sq(self);
}

/* ── step: update + sqrtf, return RMS ────────────────────────────────── */

/**
 * @brief 单步计算（更新窗口并返回 RMS）
 * @param self 对象
 * @param x 输入样本
 * @return RMS
 */
static float algo_rms_step_impl(algo_rms_t* self, float x) {
    if (self == NULL || !self->_inited)
        return 0.0f;

    float mean_sq = algo_rms_update_sq_impl(self, x);
    self->_y = sqrtf(mean_sq);
    return self->_y;
}

/* ── Reset ────────────────────────────────────────────────────────────── */

/**
 * @brief 复位
 * @param self 对象
 */
static void algo_rms_reset_impl(algo_rms_t* self) {
    if (self == NULL || !self->_inited)
        return;

    self->_idx = 0;
    self->_count = 0;
    self->_sum_sq = 0.0f;
    self->_sum = 0.0f;
    self->_y = 0.0f;
    self->_filled = false;

    for (uint16_t i = 0; i < self->_size; i++) {
        self->_buf[i] = 0.0f;
    }
}

/* ── get_rms ──────────────────────────────────────────────────────────── */

/**
 * @brief 读取当前 RMS
 * @param self 对象
 * @return RMS
 */
static float algo_rms_get_rms_impl(const algo_rms_t* self) {
    if (self == NULL || !self->_inited)
        return 0.0f;
    return self->_y;
}

/* ── get_mean_sq ──────────────────────────────────────────────────────── */

/**
 * @brief 读取当前均方值
 * @param self 对象
 * @return 均方值
 */
static float algo_rms_get_mean_sq_impl(const algo_rms_t* self) {
    if (self == NULL || !self->_inited)
        return 0.0f;

    return algo_rms_calc_mean_sq(self);
}

/* ── Constructor ──────────────────────────────────────────────────────── */

/**
 * @brief 构造 RMS 对象
 * @param self 对象
 */
void algo_rms_ctor(algo_rms_t* self) {
    if (self == NULL)
        return;

    self->init = algo_rms_init_impl;
    self->step = algo_rms_step_impl;
    self->reset = algo_rms_reset_impl;
    self->get_rms = algo_rms_get_rms_impl;
    self->get_mean_sq = algo_rms_get_mean_sq_impl;
    self->update_sq = algo_rms_update_sq_impl;
    self->_buf = NULL;
    self->_size = 0;
    self->_count = 0;
    self->_inv_size = 0.0f;
    self->_idx = 0;
    self->_sum_sq = 0.0f;
    self->_sum = 0.0f;
    self->_y = 0.0f;
    self->_remove_dc = false;
    self->_filled = false;
    self->_inited = false;
}
