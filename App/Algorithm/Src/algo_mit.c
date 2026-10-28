/**
 * @file    algo_mit.c
 * @brief   MIT 阻抗控制（PD + 前馈）纯算法实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "algo_mit.h"

#include "foc_math.h" /* FOC_ATTR_RAMFUNC */

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/** @brief 有限性判定（位模式；不依赖 isfinite，兼容 -ffast-math 测试档） */
FOC_ATTR_RAMFUNC
static bool mit_finite(float x) {
    union {
        float f;
        uint32_t u;
    } v;

    v.f = x;
    return (v.u & 0x7F800000U) != 0x7F800000U;
}

/**
 * @brief 以最大步长逼近目标值
 * @param current 当前值
 * @param target 目标值
 * @param max_step 本拍允许的最大变化量（正数）
 * @return 逼近后的值
 */
FOC_ATTR_RAMFUNC
static float mit_approach(float current, float target, float max_step) {
    float delta = target - current;

    if (delta > max_step) {
        return current + max_step;
    }
    if (delta < -max_step) {
        return current - max_step;
    }
    return target;
}

void algo_mit_reset(algo_mit_state_t* st) {
    if (st == NULL) {
        return;
    }
    *st = (algo_mit_state_t){0};
}

FOC_ATTR_RAMFUNC
int algo_mit_step(const algo_mit_cfg_t* cfg, algo_mit_state_t* st, const algo_mit_cmd_t* cmd,
                  float pos_rad, float vel_rad_s, float torque_limit_nm, float dt) {
    if ((cfg == NULL) || (st == NULL) || (cmd == NULL)) {
        return ALGO_MIT_BAD_PARAM;
    }
    /* 入口校验：非有限值/负增益/非法限幅一律拒绝，不做猜测 */
    if (!mit_finite(cmd->p_des_rad) || !mit_finite(cmd->v_des_rad_s)
        || !mit_finite(cmd->kp_nm_per_rad) || !mit_finite(cmd->kd_nm_per_rad_s)
        || !mit_finite(cmd->t_ff_nm)) {
        return ALGO_MIT_BAD_PARAM;
    }
    if ((cmd->kp_nm_per_rad < 0.0f) || (cmd->kd_nm_per_rad_s < 0.0f)) {
        return ALGO_MIT_BAD_PARAM;
    }
    if (!mit_finite(pos_rad) || !mit_finite(vel_rad_s) || !mit_finite(torque_limit_nm)
        || !mit_finite(dt) || (torque_limit_nm <= 0.0f) || (dt <= 0.0f)) {
        return ALGO_MIT_BAD_PARAM;
    }
    /* 增益上限：超限直接拒绝（不静默截断——静默截断会让主机以为刚度已生效） */
    if ((cmd->kp_nm_per_rad > cfg->kp_max_nm_per_rad)
        || (cmd->kd_nm_per_rad_s > cfg->kd_max_nm_per_rad_s)) {
        st->gain_limited = true;
        return ALGO_MIT_GAIN_LIMIT;
    }
    st->gain_limited = false;

    /* 增益斜坡：首次进入从 0 软启动；Kd 按 Kp/Kd 上限比例使用同一时间常数 */
    if (mit_finite(cfg->gain_ramp_rate) && (cfg->gain_ramp_rate > 0.0f)) {
        float kp_step = cfg->gain_ramp_rate * dt;
        float kd_rate = cfg->gain_ramp_rate;

        if (cfg->kp_max_nm_per_rad > 0.0f) {
            kd_rate = cfg->gain_ramp_rate * (cfg->kd_max_nm_per_rad_s / cfg->kp_max_nm_per_rad);
        }
        st->kp_eff_nm_per_rad =
            mit_approach(st->kp_eff_nm_per_rad, cmd->kp_nm_per_rad, kp_step);
        st->kd_eff_nm_per_rad_s =
            mit_approach(st->kd_eff_nm_per_rad_s, cmd->kd_nm_per_rad_s, kd_rate * dt);
    } else {
        st->kp_eff_nm_per_rad = cmd->kp_nm_per_rad;
        st->kd_eff_nm_per_rad_s = cmd->kd_nm_per_rad_s;
    }

    /* 位置误差：钳制而非拒绝——运行中被外力推开时不应突然失去输出 */
    float pos_err = cmd->p_des_rad - pos_rad;

    if (!mit_finite(pos_err)) {
        return ALGO_MIT_BAD_PARAM;
    }
    st->err_clamped = false;
    /* 显式比较代替 fabsf：30kHz/15kHz ISR 路径内不引入 libm 调用 */
    float abs_pos_err = (pos_err < 0.0f) ? -pos_err : pos_err;

    if ((cfg->pos_err_max_rad > 0.0f) && (abs_pos_err > cfg->pos_err_max_rad)) {
        pos_err = (pos_err > 0.0f) ? cfg->pos_err_max_rad : -cfg->pos_err_max_rad;
        st->err_clamped = true;
    }
    st->pos_err_rad = pos_err;

    /* 前馈占比限幅 */
    float t_ff = cmd->t_ff_nm;

    if (!(cfg->t_ff_max_ratio > 0.0f)) {
        t_ff = 0.0f;
    } else if (cfg->t_ff_max_ratio < 1.0f) {
        float ff_limit = cfg->t_ff_max_ratio * torque_limit_nm;

        if (t_ff > ff_limit) {
            t_ff = ff_limit;
        } else if (t_ff < -ff_limit) {
            t_ff = -ff_limit;
        }
    }

    float torque = st->kp_eff_nm_per_rad * pos_err
                   + st->kd_eff_nm_per_rad_s * (cmd->v_des_rad_s - vel_rad_s) + t_ff;

    if (!mit_finite(torque)) {
        return ALGO_MIT_BAD_PARAM;
    }
    st->saturated = false;
    if (torque > torque_limit_nm) {
        torque = torque_limit_nm;
        st->saturated = true;
    } else if (torque < -torque_limit_nm) {
        torque = -torque_limit_nm;
        st->saturated = true;
    }
    st->torque_nm = torque;
    return ALGO_MIT_OK;
}