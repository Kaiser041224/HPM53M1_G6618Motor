/**
 * @file    algo_pll.c
 * @brief   软件锁相环（周期跟踪 / 相位锁定）实现
 * @author  Kaiser
 *
 * Software PLL Implementation
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "algo_pll.h"

#include <math.h>
#include <stddef.h>

static int algo_pll_init_impl(algo_pll_t* self, const algo_pll_cfg_t* cfg);
static float algo_pll_step_impl(algo_pll_t* self, float period);
static void algo_pll_reset_impl(algo_pll_t* self);
static float algo_pll_get_freq_impl(const algo_pll_t* self);
static float algo_pll_get_period_impl(const algo_pll_t* self);
static float algo_pll_step_error_impl(algo_pll_t* self, float error);
static float algo_pll_step_phase_impl(algo_pll_t* self, float phase_meas);
static float algo_pll_step_sincos_impl(algo_pll_t* self, float sin_meas, float cos_meas);
static float algo_pll_get_phase_impl(const algo_pll_t* self);
static float algo_pll_get_omega_impl(const algo_pll_t* self);
static float algo_pll_get_sin_impl(const algo_pll_t* self);
static float algo_pll_get_cos_impl(const algo_pll_t* self);
static float algo_pll_get_phase_err_impl(const algo_pll_t* self);
static int algo_pll_set_phase_impl(algo_pll_t* self, float phase_rad);
static int algo_pll_set_freq_impl(algo_pll_t* self, float freq_hz);

static float algo_pll_wrap_pi(float x);
static float algo_pll_wrap_2pi(float x);
static void algo_pll_update_sincos(algo_pll_t* self);
static float algo_pll_step_period_tracker(algo_pll_t* self, float period_meas);
static float algo_pll_step_phase_pll(algo_pll_t* self, float phase_err);

/* ── Phase wrap ──────────────────────────────────────────────────────── */

/**
 * @brief 相位归一化到 [-π, π)
 * @param x 输入相位 [rad]
 * @return 归一化相位 [rad]
 */
static float algo_pll_wrap_pi(float x) {
    while (x >= ALGO_PLL_PI_F)
        x -= ALGO_PLL_TWO_PI_F;
    while (x < -ALGO_PLL_PI_F)
        x += ALGO_PLL_TWO_PI_F;
    return x;
}

/**
 * @brief 相位归一化到 [0, 2π)
 * @param x 输入相位 [rad]
 * @return 归一化相位 [rad]
 */
static float algo_pll_wrap_2pi(float x) {
    while (x >= ALGO_PLL_TWO_PI_F)
        x -= ALGO_PLL_TWO_PI_F;
    while (x < 0.0f)
        x += ALGO_PLL_TWO_PI_F;
    return x;
}

/* ── sin/cos update ──────────────────────────────────────────────────── */

/**
 * @brief 更新内部 sin/cos 输出
 * @param self 对象
 */
static void algo_pll_update_sincos(algo_pll_t* self) {
#if ALGO_PLL_ENABLE_SINCOS_OUTPUT
    float phase = self->_phase + self->_phase_offset;
    self->_sin = sinf(phase);
    self->_cos = cosf(phase);
#else
    (void)self;
#endif
}

/* ── Init ─────────────────────────────────────────────────────────────── */

/**
 * @brief 初始化锁相环
 * @param self 对象
 * @param cfg 配置
 * @return 0 成功；负数错误码
 */
static int algo_pll_init_impl(algo_pll_t* self, const algo_pll_cfg_t* cfg) {
    if (self != NULL)
        self->_inited = false;

    if (self == NULL)
        return -1;
    if (cfg == NULL)
        return -2;
    if (!algo_pll_finite(cfg->kp))
        return -3;
    if (!algo_pll_finite(cfg->ki))
        return -4;

    self->_kp = cfg->kp;
    self->_ki = cfg->ki;

    switch (cfg->mode) {

    case ALGO_PLL_MODE_PERIOD_TRACKER: {
        if (!algo_pll_finite(cfg->period_nominal))
            return -5;
        if (!algo_pll_finite(cfg->period_min))
            return -6;
        if (!algo_pll_finite(cfg->period_max))
            return -7;
        if (cfg->period_nominal <= 0.0f)
            return -5;
        if (cfg->period_min >= cfg->period_max)
            return -8;
        if (cfg->period_nominal < cfg->period_min || cfg->period_nominal > cfg->period_max)
            return -9;

        self->_mode = ALGO_PLL_MODE_PERIOD_TRACKER;
        self->_period_nominal = cfg->period_nominal;
        self->_period_min = cfg->period_min;
        self->_period_max = cfg->period_max;
        self->_period = cfg->period_nominal;
        self->_integral = 0.0f;
        self->_freq = 1.0f / cfg->period_nominal;
        self->_ts_s = 0.0f;
    } break;

    case ALGO_PLL_MODE_PHASE_LOCKED: {
        if (!algo_pll_finite(cfg->sample_time_s))
            return -10;
        if (cfg->sample_time_s <= 0.0f)
            return -10;
        if (!algo_pll_finite(cfg->freq_nominal_hz))
            return -11;
        if (cfg->freq_nominal_hz <= 0.0f)
            return -11;
        if (!algo_pll_finite(cfg->freq_min_hz))
            return -12;
        if (cfg->freq_min_hz <= 0.0f)
            return -12;
        if (!algo_pll_finite(cfg->freq_max_hz))
            return -13;
        if (cfg->freq_max_hz <= cfg->freq_min_hz)
            return -13;
        if (cfg->freq_nominal_hz < cfg->freq_min_hz || cfg->freq_nominal_hz > cfg->freq_max_hz)
            return -14;
        if (!algo_pll_finite(cfg->phase_init_rad))
            return -15;
        if (!algo_pll_finite(cfg->phase_offset_rad))
            return -16;
        if (cfg->kp < 0.0f)
            return -3;
        if (cfg->ki < 0.0f)
            return -4;

        self->_mode = ALGO_PLL_MODE_PHASE_LOCKED;
        self->_ts_s = cfg->sample_time_s;
        self->_freq_nominal = cfg->freq_nominal_hz;
        self->_freq_min = cfg->freq_min_hz;
        self->_freq_max = cfg->freq_max_hz;
        self->_phase_init = algo_pll_wrap_2pi(cfg->phase_init_rad);
        self->_phase_offset = cfg->phase_offset_rad;

        self->_freq = cfg->freq_nominal_hz;
        self->_period = 1.0f / cfg->freq_nominal_hz;
        self->_omega_nominal = ALGO_PLL_TWO_PI_F * cfg->freq_nominal_hz;
        self->_omega_min = ALGO_PLL_TWO_PI_F * cfg->freq_min_hz;
        self->_omega_max = ALGO_PLL_TWO_PI_F * cfg->freq_max_hz;
        self->_omega = self->_omega_nominal;
        self->_phase = self->_phase_init;
        self->_phase_err = 0.0f;
        self->_loop_integral = 0.0f;
        self->_period_nominal = 0.0f;
        self->_period_min = 0.0f;
        self->_period_max = 0.0f;
        self->_integral = 0.0f;

        algo_pll_update_sincos(self);
    } break;

    default: return -17;
    }

    self->_inited = true;
    return 0;
}

/* ── Step: period tracker (backward compatible) ──────────────────────── */

/**
 * @brief 周期跟踪单步
 * @param self 对象
 * @param period_meas 测量周期 [s]
 * @return 跟踪频率 [Hz]
 */
static float algo_pll_step_period_tracker(algo_pll_t* self, float period_meas) {
    if (!algo_pll_finite(period_meas) || period_meas <= 0.0f)
        return self->_freq;

    float err = period_meas - self->_period;
    self->_integral += self->_ki * err;

    if (!algo_pll_finite(self->_integral))
        self->_integral = 0.0f;

    float period = self->_period_nominal + self->_kp * err + self->_integral;

    if (period < self->_period_min) {
        period = self->_period_min;
        self->_integral = period - self->_period_nominal - self->_kp * err;
    } else if (period > self->_period_max) {
        period = self->_period_max;
        self->_integral = period - self->_period_nominal - self->_kp * err;
    }

    self->_period = period;

    if (self->_period > 0.0f) {
        self->_freq = 1.0f / self->_period;
        self->_omega = ALGO_PLL_TWO_PI_F * self->_freq;
    }

    if (!algo_pll_finite(self->_freq))
        self->_freq = 0.0f;

    return self->_freq;
}

/* ── Step: phase-locked PLL (NCO update) ─────────────────────────────── */

/**
 * @brief 相位锁定单步（NCO 更新）
 * @param self 对象
 * @param phase_err 相位误差 [rad]
 * @return 跟踪频率 [Hz]
 */
static float algo_pll_step_phase_pll(algo_pll_t* self, float phase_err) {
    phase_err = algo_pll_wrap_pi(phase_err);

    if (!algo_pll_finite(phase_err))
        return self->_freq;

    float sample_time_s = self->_ts_s;
    self->_loop_integral += self->_ki * sample_time_s * phase_err;
    if (!algo_pll_finite(self->_loop_integral))
        self->_loop_integral = 0.0f;

    float omega_cmd = self->_omega_nominal + self->_kp * phase_err + self->_loop_integral;

    if (omega_cmd < self->_omega_min) {
        omega_cmd = self->_omega_min;
        self->_loop_integral = omega_cmd - self->_omega_nominal - self->_kp * phase_err;
    } else if (omega_cmd > self->_omega_max) {
        omega_cmd = self->_omega_max;
        self->_loop_integral = omega_cmd - self->_omega_nominal - self->_kp * phase_err;
    }

    self->_phase_err = phase_err;
    self->_omega = omega_cmd;
    self->_freq = omega_cmd / ALGO_PLL_TWO_PI_F;
    self->_period = 1.0f / self->_freq;

    if (!algo_pll_finite(self->_freq)) {
        self->_freq = self->_freq_nominal;
        self->_period = 1.0f / self->_freq;
        return self->_freq;
    }

    self->_phase += omega_cmd * sample_time_s;
    self->_phase = algo_pll_wrap_2pi(self->_phase);

    algo_pll_update_sincos(self);

    return self->_freq;
}

/* ── Step dispatcher ─────────────────────────────────────────────────── */

/**
 * @brief 单步分派（周期跟踪 / 相位锁定）
 * @param self 对象
 * @param period 测量周期 [s]
 * @return 跟踪频率 [Hz]
 */
static float algo_pll_step_impl(algo_pll_t* self, float period) {
    if (self == NULL || !self->_inited)
        return 0.0f;

    if (self->_mode == ALGO_PLL_MODE_PHASE_LOCKED) {
        (void)period;
        return self->_freq;
    }

    return algo_pll_step_period_tracker(self, period);
}

/* ── step_error ──────────────────────────────────────────────────────── */

/**
 * @brief 相位误差单步
 * @param self 对象
 * @param error 周期或相位误差（按模式解释）
 * @return 跟踪频率 [Hz]
 */
static float algo_pll_step_error_impl(algo_pll_t* self, float error) {
    if (self == NULL || !self->_inited)
        return 0.0f;

    if (self->_mode == ALGO_PLL_MODE_PERIOD_TRACKER) {
        return algo_pll_step_period_tracker(self, error);
    }

    return algo_pll_step_phase_pll(self, error);
}

/* ── step_phase ──────────────────────────────────────────────────────── */

/**
 * @brief 相位测量单步
 * @param self 对象
 * @param phase_meas 测量相位 [rad]
 * @return 跟踪频率 [Hz]
 */
static float algo_pll_step_phase_impl(algo_pll_t* self, float phase_meas) {
    if (self == NULL || !self->_inited)
        return 0.0f;

    if (!algo_pll_finite(phase_meas))
        return self->_freq;

    float err = algo_pll_wrap_pi(phase_meas - self->_phase);

    if (self->_mode == ALGO_PLL_MODE_PERIOD_TRACKER) {
        return algo_pll_step_period_tracker(self, err);
    }

    return algo_pll_step_phase_pll(self, err);
}

/* ── step_sincos ─────────────────────────────────────────────────────── */

/**
 * @brief 正余弦测量单步
 * @param self 对象
 * @param sin_meas 测量 sin
 * @param cos_meas 测量 cos
 * @return 跟踪频率 [Hz]
 */
static float algo_pll_step_sincos_impl(algo_pll_t* self, float sin_meas, float cos_meas) {
    if (self == NULL || !self->_inited)
        return 0.0f;

    (void)sin_meas;
    (void)cos_meas;

#if ALGO_PLL_ENABLE_ATAN2_INPUT
    if (!algo_pll_finite(sin_meas) || !algo_pll_finite(cos_meas))
        return self->_freq;

    float phase_meas = atan2f(sin_meas, cos_meas);

    float err = algo_pll_wrap_pi(phase_meas - self->_phase);

    if (self->_mode == ALGO_PLL_MODE_PERIOD_TRACKER) {
        return algo_pll_step_period_tracker(self, err);
    }

    return algo_pll_step_phase_pll(self, err);
#else
    return self->_freq;
#endif
}

/* ── Reset ────────────────────────────────────────────────────────────── */

/**
 * @brief 复位锁相环
 * @param self 对象
 */
static void algo_pll_reset_impl(algo_pll_t* self) {
    if (self == NULL || !self->_inited)
        return;

    if (self->_mode == ALGO_PLL_MODE_PHASE_LOCKED) {
        self->_phase = self->_phase_init;
        self->_freq = self->_freq_nominal;
        self->_period = 1.0f / self->_freq;
        self->_omega = self->_omega_nominal;
        self->_loop_integral = 0.0f;
        self->_phase_err = 0.0f;
        algo_pll_update_sincos(self);
    } else {
        self->_period = self->_period_nominal;
        self->_integral = 0.0f;
        self->_freq = 1.0f / self->_period_nominal;
    }
}

/* ── Getters ──────────────────────────────────────────────────────────── */

/**
 * @brief 读取跟踪频率
 * @param self 对象
 * @return 频率 [Hz]
 */
static float algo_pll_get_freq_impl(const algo_pll_t* self) {
    if (self == NULL || !self->_inited)
        return 0.0f;
    return self->_freq;
}

/**
 * @brief 读取跟踪周期
 * @param self 对象
 * @return 周期 [s]
 */
static float algo_pll_get_period_impl(const algo_pll_t* self) {
    if (self == NULL || !self->_inited)
        return 0.0f;
    return self->_period;
}

/**
 * @brief 读取锁相相位
 * @param self 对象
 * @return 相位 [rad]
 */
static float algo_pll_get_phase_impl(const algo_pll_t* self) {
    if (self == NULL || !self->_inited)
        return 0.0f;
    return self->_phase;
}

/**
 * @brief 读取角频率
 * @param self 对象
 * @return 角频率 [rad/s]
 */
static float algo_pll_get_omega_impl(const algo_pll_t* self) {
    if (self == NULL || !self->_inited)
        return 0.0f;
    return self->_omega;
}

/**
 * @brief 读取内部 sin
 * @param self 对象
 * @return sin 值
 */
static float algo_pll_get_sin_impl(const algo_pll_t* self) {
#if ALGO_PLL_ENABLE_SINCOS_OUTPUT
    if (self == NULL || !self->_inited)
        return 0.0f;
    return self->_sin;
#else
    (void)self;
    return 0.0f;
#endif
}

/**
 * @brief 读取内部 cos
 * @param self 对象
 * @return cos 值
 */
static float algo_pll_get_cos_impl(const algo_pll_t* self) {
#if ALGO_PLL_ENABLE_SINCOS_OUTPUT
    if (self == NULL || !self->_inited)
        return 0.0f;
    return self->_cos;
#else
    (void)self;
    return 0.0f;
#endif
}

/**
 * @brief 读取相位误差
 * @param self 对象
 * @return 相位误差 [rad]
 */
static float algo_pll_get_phase_err_impl(const algo_pll_t* self) {
    if (self == NULL || !self->_inited)
        return 0.0f;
    return self->_phase_err;
}

/* ── Setters ──────────────────────────────────────────────────────────── */

/**
 * @brief 设置相位
 * @param self 对象
 * @param phase_rad 目标相位 [rad]
 * @return 0 成功；负数错误码
 */
static int algo_pll_set_phase_impl(algo_pll_t* self, float phase_rad) {
    if (self == NULL || !self->_inited)
        return -1;
    if (!algo_pll_finite(phase_rad))
        return -2;

    self->_phase = algo_pll_wrap_2pi(phase_rad);
    algo_pll_update_sincos(self);
    return 0;
}

/**
 * @brief 设置频率
 * @param self 对象
 * @param freq_hz 目标频率 [Hz]
 * @return 0 成功；负数错误码
 */
static int algo_pll_set_freq_impl(algo_pll_t* self, float freq_hz) {
    if (self == NULL || !self->_inited)
        return -1;
    if (!algo_pll_finite(freq_hz))
        return -2;

    if (self->_mode == ALGO_PLL_MODE_PHASE_LOCKED) {
        if (freq_hz < self->_freq_min || freq_hz > self->_freq_max)
            return -3;
    } else {
        float period = 1.0f / freq_hz;
        if (period < self->_period_min || period > self->_period_max)
            return -3;
    }

    self->_freq = freq_hz;
    self->_period = 1.0f / freq_hz;
    if (!algo_pll_finite(self->_period))
        return -4;

    if (self->_mode == ALGO_PLL_MODE_PHASE_LOCKED) {
        self->_omega = ALGO_PLL_TWO_PI_F * freq_hz;
    }

    return 0;
}

/* ── Constructor ──────────────────────────────────────────────────────── */

/**
 * @brief 构造锁相环对象
 * @param self 对象
 */
void algo_pll_ctor(algo_pll_t* self) {
    if (self == NULL)
        return;

    self->init = algo_pll_init_impl;
    self->step = algo_pll_step_impl;
    self->reset = algo_pll_reset_impl;
    self->get_freq = algo_pll_get_freq_impl;
    self->get_period = algo_pll_get_period_impl;
    self->step_error = algo_pll_step_error_impl;
    self->step_phase = algo_pll_step_phase_impl;
    self->step_sincos = algo_pll_step_sincos_impl;
    self->get_phase = algo_pll_get_phase_impl;
    self->get_omega = algo_pll_get_omega_impl;
    self->get_sin = algo_pll_get_sin_impl;
    self->get_cos = algo_pll_get_cos_impl;
    self->get_phase_error = algo_pll_get_phase_err_impl;
    self->set_phase = algo_pll_set_phase_impl;
    self->set_freq = algo_pll_set_freq_impl;

    self->_mode = ALGO_PLL_MODE_PERIOD_TRACKER;
    self->_kp = 0.0f;
    self->_ki = 0.0f;
    self->_ts_s = 0.0f;
    self->_period_nominal = 0.0f;
    self->_period_min = 0.0f;
    self->_period_max = 0.0f;
    self->_period = 0.0f;
    self->_integral = 0.0f;
    self->_freq = 0.0f;
    self->_phase = 0.0f;
    self->_phase_offset = 0.0f;
    self->_phase_err = 0.0f;
    self->_omega = 0.0f;
    self->_omega_nominal = 0.0f;
    self->_omega_min = 0.0f;
    self->_omega_max = 0.0f;
    self->_loop_integral = 0.0f;
    self->_phase_init = 0.0f;
    self->_freq_nominal = 0.0f;
    self->_freq_min = 0.0f;
    self->_freq_max = 0.0f;
#if ALGO_PLL_ENABLE_SINCOS_OUTPUT
    self->_sin = 0.0f;
    self->_cos = 0.0f;
#endif
    self->_inited = false;
}
