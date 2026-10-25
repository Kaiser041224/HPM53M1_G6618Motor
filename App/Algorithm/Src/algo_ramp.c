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
static int algo_ramp_plan_impl(algo_ramp_t* self, float target, float current, float velocity);
static float algo_ramp_get_current_impl(const algo_ramp_t* self);
static float algo_ramp_get_target_impl(const algo_ramp_t* self);
static float algo_ramp_get_velocity_impl(const algo_ramp_t* self);
static float algo_ramp_get_acceleration_impl(const algo_ramp_t* self);
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
    self->_acceleration = 0.0f;
    self->_elapsed = 0.0f;
    self->_phase = RAMP_PH_ACCEL;
    self->_done = false;

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

    /* 梯形模式自带到位/越界判据，不能先走通用 epsilon 快照（会丢掉速度状态）。 */
    if (self->_mode == ALGO_RAMP_MODE_TRAPEZOIDAL) {
        return algo_ramp_step_trapezoidal(self);
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
    const float dt = self->_ts_s;
    const float a_max = self->_a_max;
    const float v_max = self->_v_max;
    float x = self->_current;
    float v = self->_velocity;
    float dx = self->_target - x;
    float v_abs = fabsf(v);
    float accel = 0.0f;

    if (!algo_ramp_finite(x) || !algo_ramp_finite(v) || !algo_ramp_finite(dx)) {
        self->_current = self->_target;
        self->_velocity = 0.0f;
        self->_acceleration = 0.0f;
        self->_done = true;
        self->_phase = RAMP_PH_DONE;
        return self->_current;
    }

    if ((fabsf(dx) <= ALGO_RAMP_EPSILON) && (v_abs <= ALGO_RAMP_EPSILON)) {
        self->_current = self->_target;
        self->_velocity = 0.0f;
        self->_acceleration = 0.0f;
        self->_done = true;
        self->_phase = RAMP_PH_DONE;
        return self->_current;
    }

    /* 有符号时间最优梯形：先保证不越过目标，再按剩余距离决定加速/巡航/制动。
     * 使用有符号速度而不是“目标方向 × 正速度”，支持运动中反向和新的位置目标。 */
    float brake_distance = (v_abs * v_abs) / (2.0f * a_max);
    bool moving_toward_target = ((dx * v) > 0.0f);
    bool braking = false;

    if (v_abs <= ALGO_RAMP_EPSILON) {
        accel = (dx > 0.0f) ? a_max : -a_max;
    } else if (v_abs > v_max) {
        accel = (v > 0.0f) ? -a_max : a_max;
        braking = true;
    } else if (!moving_toward_target) {
        accel = (v > 0.0f) ? -a_max : a_max;
        braking = true;
    } else if (fabsf(dx) <= brake_distance) {
        accel = (v > 0.0f) ? -a_max : a_max;
        braking = true;
    } else {
        accel = (dx > 0.0f) ? a_max : -a_max;
    }

    float next_v = v + accel * dt;
    if (braking && (((v > 0.0f) && (next_v < 0.0f)) || ((v < 0.0f) && (next_v > 0.0f)))) {
        next_v = 0.0f; /* 制动只到零，不在同一拍反向 */
    }
    if ((next_v > v_max) && (accel > 0.0f)) {
        next_v = v_max;
    } else if ((next_v < -v_max) && (accel < 0.0f)) {
        next_v = -v_max;
    }

    float next_x = x + next_v * dt;
    bool crossed_target = ((dx > 0.0f) && (next_x >= self->_target))
                       || ((dx < 0.0f) && (next_x <= self->_target));
    if (crossed_target) {
        self->_current = self->_target;
        self->_velocity = 0.0f;
        self->_acceleration = 0.0f;
        self->_done = true;
        self->_phase = RAMP_PH_DONE;
        return self->_current;
    }

    /* Ydd 必须是本拍“实际”速度变化，而不是命令加速度：v_max 钳位、
     * 制动过零截断都会让实际加速度小于/不同于 accel。惯量前馈直接消费
     * get_acceleration()，用命令值会在巡航段持续输出虚假力矩。 */
    float actual_accel = (next_v - v) / dt;

    self->_current = next_x;
    self->_velocity = next_v;
    self->_acceleration = actual_accel;
    self->_done = false;
    if (actual_accel == 0.0f) {
        self->_phase = RAMP_PH_CRUISE;
    } else if ((actual_accel > 0.0f) == (next_v > 0.0f)) {
        self->_phase = RAMP_PH_ACCEL;
    } else {
        self->_phase = RAMP_PH_DECEL;
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
    self->_acceleration = 0.0f;
    self->_elapsed = 0.0f;
    self->_phase = RAMP_PH_ACCEL;
    self->_done = false;
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
    self->_acceleration = 0.0f;
    self->_done = false;
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
    self->_acceleration = 0.0f;
    self->_elapsed = 0.0f;
    self->_phase = RAMP_PH_ACCEL;
    self->_done = false;
    return 0;
}

/* ── plan ─────────────────────────────────────────────────────────────── */

/**
 * @brief 带初速度重规划（梯形模式保留有符号初速度；其他模式忽略速度）
 * @param self 对象
 * @param target 目标值
 * @param current 当前值
 * @param velocity 当前速度
 * @return 0 成功；负数错误码
 */
static int algo_ramp_plan_impl(algo_ramp_t* self, float target, float current, float velocity) {
    if (self == NULL || !self->_inited) {
        return -1;
    }
    if (!algo_ramp_finite(target) || !algo_ramp_finite(current)
        || !algo_ramp_finite(velocity)) {
        return -2;
    }

    self->_target = target;
    self->_current = current;
    self->_initial = current;
    self->_elapsed = 0.0f;
    self->_phase = RAMP_PH_ACCEL;
    self->_done = false;

    if (self->_mode == ALGO_RAMP_MODE_TRAPEZOIDAL) {
        self->_velocity = velocity;
        self->_acceleration = 0.0f;
    } else {
        self->_velocity = 0.0f;
        self->_acceleration = 0.0f;
        if (self->_mode == ALGO_RAMP_MODE_SMOOTHSTEP) {
            self->_initial = current;
        }
    }
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
 * @brief 读取本拍速度 Yd
 * @param self 对象
 * @return 速度；非梯形模式为 0
 */
static float algo_ramp_get_velocity_impl(const algo_ramp_t* self) {
    if (self == NULL || !self->_inited || (self->_mode != ALGO_RAMP_MODE_TRAPEZOIDAL)) {
        return 0.0f;
    }
    return self->_velocity;
}

/**
 * @brief 读取本拍加速度 Ydd
 * @param self 对象
 * @return 加速度；非梯形模式为 0
 */
static float algo_ramp_get_acceleration_impl(const algo_ramp_t* self) {
    if (self == NULL || !self->_inited || (self->_mode != ALGO_RAMP_MODE_TRAPEZOIDAL)) {
        return 0.0f;
    }
    return self->_acceleration;
}

/**
 * @brief 查询是否到位
 * @param self 对象
 * @return true = 已到位
 */
static bool algo_ramp_is_done_impl(const algo_ramp_t* self) {
    if (self == NULL || !self->_inited)
        return false;
    if (self->_mode == ALGO_RAMP_MODE_TRAPEZOIDAL) {
        return self->_done;
    }
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
    self->plan = algo_ramp_plan_impl;
    self->get_current = algo_ramp_get_current_impl;
    self->get_target = algo_ramp_get_target_impl;
    self->get_velocity = algo_ramp_get_velocity_impl;
    self->get_acceleration = algo_ramp_get_acceleration_impl;
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
    self->_acceleration = 0.0f;
    self->_elapsed = 0.0f;
    self->_duration = 0.0f;
    self->_phase = 0;
    self->_done = false;
    self->_inited = false;
}
