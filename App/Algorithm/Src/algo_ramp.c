/**
 * @file    algo_ramp.c
 * @brief   斜坡发生器（线性 / 指数 / 梯形 / 平滑阶跃）实现
 * @author  Kaiser
 *
 * Ramp Generator Implementation
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "algo_ramp.h"

#include <math.h>
#include <stddef.h>

#define RAMP_PH_ACCEL  0
#define RAMP_PH_CRUISE 1
#define RAMP_PH_DECEL  2
#define RAMP_PH_DONE   3

static int algo_ramp_init_impl(algo_ramp_t* self, const algo_ramp_cfg_t* cfg);
static float algo_ramp_step_impl(algo_ramp_t* self);
static void algo_ramp_reset_impl(algo_ramp_t* self);
static int algo_ramp_set_target_impl(algo_ramp_t* self, float target);
static int algo_ramp_set_current_impl(algo_ramp_t* self, float current);
static float algo_ramp_get_current_impl(const algo_ramp_t* self);
static float algo_ramp_get_target_impl(const algo_ramp_t* self);
static bool algo_ramp_is_done_impl(const algo_ramp_t* self);

static float algo_ramp_step_linear(algo_ramp_t* self);
static float algo_ramp_step_exponential(algo_ramp_t* self);
static float algo_ramp_step_trapezoidal(algo_ramp_t* self);
static float algo_ramp_step_smoothstep(algo_ramp_t* self);

/* ── Init ─────────────────────────────────────────────────────────────── */

/**
 * @brief 初始化斜坡发生器
 * @param self 对象
 * @param cfg 配置
 * @return 0 成功；负数错误码
 */
static int algo_ramp_init_impl(algo_ramp_t* self, const algo_ramp_cfg_t* cfg) {
    if (self == NULL)
        return -1;
    if (cfg == NULL)
        return -2;
    if (!algo_ramp_finite(cfg->step_time_s))
        return -8;
    if (cfg->step_time_s <= 0.0f)
        return -3;
    if (!algo_ramp_finite(cfg->rate))
        return -7;
    if (cfg->rate <= 0.0f)
        return -4;
    if (!algo_ramp_finite(cfg->target))
        return -5;
    if (!algo_ramp_finite(cfg->initial))
        return -6;

    switch (cfg->mode) {
    case ALGO_RAMP_MODE_LINEAR:
    case ALGO_RAMP_MODE_EXPONENTIAL:
    case ALGO_RAMP_MODE_TRAPEZOIDAL:
    case ALGO_RAMP_MODE_SMOOTHSTEP: break;
    default: self->_inited = false; return -9;
    }

    self->_mode = cfg->mode;
    self->_target = cfg->target;
    self->_initial = cfg->initial;
    self->_current = cfg->initial;
    self->_ts_s = cfg->step_time_s;
    self->_velocity = 0.0f;
    self->_elapsed = 0.0f;
    self->_phase = RAMP_PH_ACCEL;

    switch (cfg->mode) {

    case ALGO_RAMP_MODE_EXPONENTIAL: {
        float arg = -cfg->step_time_s / cfg->rate;
        if (!algo_ramp_finite(arg)) {
            self->_inited = false;
            return -10;
        }
        self->_alpha = 1.0f - expf(arg);
        if (!algo_ramp_finite(self->_alpha)) {
            self->_inited = false;
            return -10;
        }
        if (self->_alpha < 0.0f)
            self->_alpha = 0.0f;
        if (self->_alpha > 1.0f)
            self->_alpha = 1.0f;
        self->_inc = 0.0f;
        self->_a_max = 0.0f;
        self->_v_max = 0.0f;
        self->_duration = 0.0f;
    } break;

    case ALGO_RAMP_MODE_TRAPEZOIDAL: {
        if (!algo_ramp_finite(cfg->accel_max)) {
            self->_inited = false;
            return -11;
        }
        if (cfg->accel_max <= 0.0f) {
            self->_inited = false;
            return -11;
        }
        self->_v_max = cfg->rate;
        self->_a_max = cfg->accel_max;
        self->_inc = 0.0f;
        self->_alpha = 0.0f;
        self->_duration = 0.0f;
    } break;

    case ALGO_RAMP_MODE_SMOOTHSTEP: {
        if (cfg->duration_s > 0.0f) {
            if (!algo_ramp_finite(cfg->duration_s)) {
                self->_inited = false;
                return -12;
            }
            self->_duration = cfg->duration_s;
        } else {
            float dist = (cfg->target > cfg->initial) ? (cfg->target - cfg->initial)
                                                      : (cfg->initial - cfg->target);
            self->_duration = dist / cfg->rate;
        }
        if (!algo_ramp_finite(self->_duration)) {
            self->_inited = false;
            return -12;
        }
        if (self->_duration <= 0.0f) {
            self->_inited = false;
            return -12;
        }
        self->_inc = 0.0f;
        self->_alpha = 0.0f;
        self->_a_max = 0.0f;
        self->_v_max = 0.0f;
    } break;

    default: /* LINEAR */
        self->_inc = cfg->rate * cfg->step_time_s;
        if (!algo_ramp_finite(self->_inc)) {
            self->_inited = false;
            return -10;
        }
        if (self->_inc <= 0.0f) {
            self->_inited = false;
            return -10;
        }
        self->_alpha = 0.0f;
        self->_a_max = 0.0f;
        self->_v_max = 0.0f;
        self->_duration = 0.0f;
        break;
    }

    self->_inited = true;
    return 0;
}

/* ── Step dispatcher ─────────────────────────────────────────────────── */

/**
 * @brief 单步分派（按工作模式）
 * @param self 对象
 * @return 当前输出
 */
static float algo_ramp_step_impl(algo_ramp_t* self) {
    if (self == NULL || !self->_inited)
        return 0.0f;

    if (!algo_ramp_finite(self->_current) || !algo_ramp_finite(self->_target)) {
        if (algo_ramp_finite(self->_initial)) {
            self->_current = self->_initial;
            return self->_initial;
        }
        return 0.0f;
    }

    float err = self->_target - self->_current;
    float abs_err = (err < 0.0f) ? -err : err;
    if (abs_err <= ALGO_RAMP_EPSILON) {
        self->_current = self->_target;
        self->_phase = RAMP_PH_DONE;
        return self->_target;
    }

    switch (self->_mode) {
    case ALGO_RAMP_MODE_EXPONENTIAL: return algo_ramp_step_exponential(self);
    case ALGO_RAMP_MODE_TRAPEZOIDAL: return algo_ramp_step_trapezoidal(self);
    case ALGO_RAMP_MODE_SMOOTHSTEP: return algo_ramp_step_smoothstep(self);
    default: return algo_ramp_step_linear(self);
    }
}

/* ── Linear ───────────────────────────────────────────────────────────── */

/**
 * @brief 线性模式单步
 * @param self 对象
 * @return 当前输出
 */
static float algo_ramp_step_linear(algo_ramp_t* self) {
    if (self->_current < self->_target) {
        self->_current += self->_inc;
        if (self->_current > self->_target)
            self->_current = self->_target;
    } else {
        self->_current -= self->_inc;
        if (self->_current < self->_target)
            self->_current = self->_target;
    }
    return self->_current;
}

/* ── Exponential ──────────────────────────────────────────────────────── */

/**
 * @brief 指数模式单步
 * @param self 对象
 * @return 当前输出
 */
static float algo_ramp_step_exponential(algo_ramp_t* self) {
    self->_current += self->_alpha * (self->_target - self->_current);
    return self->_current;
}

/* ── Trapezoidal ──────────────────────────────────────────────────────── */
/*
 * Phases:  ACCEL → CRUISE → DECEL → DONE
 * Decel triggered when remaining distance ≤ v² / (2 * a_max).
 */

/**
 * @brief 梯形模式单步（含加减速限制）
 * @param self 对象
 * @return 当前输出
 */
static float algo_ramp_step_trapezoidal(algo_ramp_t* self) {
    float sample_time_s = self->_ts_s;
    float a_max = self->_a_max;
    float v_max = self->_v_max;
    float dir = (self->_target > self->_current) ? 1.0f : -1.0f;

    if (self->_phase == RAMP_PH_ACCEL) {
        self->_velocity += a_max * sample_time_s;
        if (self->_velocity > v_max)
            self->_velocity = v_max;
    }

    float d_remain = (self->_target > self->_current) ? (self->_target - self->_current)
                                                      : (self->_current - self->_target);
    float d_brake = (self->_velocity * self->_velocity) / (2.0f * a_max + 1.0e-12f);

    if (d_remain <= d_brake && self->_phase != RAMP_PH_DECEL) {
        self->_phase = RAMP_PH_DECEL;
    }

    if (self->_velocity >= v_max && self->_phase == RAMP_PH_ACCEL) {
        self->_phase = RAMP_PH_CRUISE;
    }

    if (self->_phase == RAMP_PH_DECEL) {
        self->_velocity -= a_max * sample_time_s;
        if (self->_velocity < 0.0f)
            self->_velocity = 0.0f;
    }

    self->_current += dir * self->_velocity * sample_time_s;

    if (dir > 0.0f && self->_current > self->_target) {
        self->_current = self->_target;
        self->_velocity = 0.0f;
        self->_phase = RAMP_PH_DONE;
    }
    if (dir < 0.0f && self->_current < self->_target) {
        self->_current = self->_target;
        self->_velocity = 0.0f;
        self->_phase = RAMP_PH_DONE;
    }

    return self->_current;
}

/* ── Smoothstep ───────────────────────────────────────────────────────── */
/*
 * s(t) = 3t² − 2t³  (Perlin smoothstep), t = elapsed / duration ∈ [0, 1]
 */

/**
 * @brief 平滑阶跃模式单步
 * @param self 对象
 * @return 当前输出
 */
static float algo_ramp_step_smoothstep(algo_ramp_t* self) {
    self->_elapsed += self->_ts_s;
    if (self->_elapsed >= self->_duration) {
        self->_elapsed = self->_duration;
        self->_current = self->_target;
        return self->_target;
    }

    float progress = self->_elapsed / self->_duration;
    float s_curve = progress * progress * (3.0f - 2.0f * progress);

    self->_current = self->_initial + (self->_target - self->_initial) * s_curve;
    return self->_current;
}

/* ── Reset ────────────────────────────────────────────────────────────── */

/**
 * @brief 复位
 * @param self 对象
 */
static void algo_ramp_reset_impl(algo_ramp_t* self) {
    if (self == NULL || !self->_inited)
        return;
    self->_current = self->_initial;
    self->_velocity = 0.0f;
    self->_elapsed = 0.0f;
    self->_phase = RAMP_PH_ACCEL;
}

/* ── set_target ───────────────────────────────────────────────────────── */

/**
 * @brief 设置目标值
 * @param self 对象
 * @param target 目标值
 * @return 0 成功；负数错误码
 */
static int algo_ramp_set_target_impl(algo_ramp_t* self, float target) {
    if (self == NULL || !self->_inited)
        return -1;
    if (!algo_ramp_finite(target))
        return -2;
    self->_target = target;
    self->_phase = RAMP_PH_ACCEL;
    if (self->_mode == ALGO_RAMP_MODE_SMOOTHSTEP) {
        self->_elapsed = 0.0f;
        self->_initial = self->_current;
    }
    return 0;
}

/* ── set_current ──────────────────────────────────────────────────────── */

/**
 * @brief 设置当前值
 * @param self 对象
 * @param current 当前值
 * @return 0 成功；负数错误码
 */
static int algo_ramp_set_current_impl(algo_ramp_t* self, float current) {
    if (self == NULL || !self->_inited)
        return -1;
    if (!algo_ramp_finite(current))
        return -2;
    self->_current = current;
    self->_initial = current;
    self->_velocity = 0.0f;
    self->_elapsed = 0.0f;
    self->_phase = RAMP_PH_ACCEL;
    return 0;
}

/* ── getters ──────────────────────────────────────────────────────────── */

/**
 * @brief 读取当前值
 * @param self 对象
 * @return 当前值
 */
static float algo_ramp_get_current_impl(const algo_ramp_t* self) {
    if (self == NULL || !self->_inited)
        return 0.0f;
    return self->_current;
}

/**
 * @brief 读取目标值
 * @param self 对象
 * @return 目标值
 */
static float algo_ramp_get_target_impl(const algo_ramp_t* self) {
    if (self == NULL || !self->_inited)
        return 0.0f;
    return self->_target;
}

/**
 * @brief 查询是否到位
 * @param self 对象
 * @return true = 已到位
 */
static bool algo_ramp_is_done_impl(const algo_ramp_t* self) {
    if (self == NULL || !self->_inited)
        return false;
    float err = self->_target - self->_current;
    return (err <= ALGO_RAMP_EPSILON) && (err >= -ALGO_RAMP_EPSILON);
}

/* ── Constructor ──────────────────────────────────────────────────────── */

/**
 * @brief 构造斜坡发生器对象
 * @param self 对象
 */
void algo_ramp_ctor(algo_ramp_t* self) {
    if (self == NULL)
        return;

    self->init = algo_ramp_init_impl;
    self->step = algo_ramp_step_impl;
    self->reset = algo_ramp_reset_impl;
    self->set_target = algo_ramp_set_target_impl;
    self->set_current = algo_ramp_set_current_impl;
    self->get_current = algo_ramp_get_current_impl;
    self->get_target = algo_ramp_get_target_impl;
    self->is_done = algo_ramp_is_done_impl;
    self->_mode = ALGO_RAMP_MODE_LINEAR;
    self->_inc = 0.0f;
    self->_alpha = 0.0f;
    self->_a_max = 0.0f;
    self->_v_max = 0.0f;
    self->_ts_s = 0.0f;
    self->_target = 0.0f;
    self->_initial = 0.0f;
    self->_current = 0.0f;
    self->_velocity = 0.0f;
    self->_elapsed = 0.0f;
    self->_duration = 0.0f;
    self->_phase = 0;
    self->_inited = false;
}
