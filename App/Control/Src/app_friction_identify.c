/**
 * @file    app_friction_identify.c
 * @brief   摩擦前馈自动辨识状态机实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_friction_identify.h"

#include "app_foc.h"
#include "app_motion.h"
#include "app_software_params.h"

#include <math.h>
#include <string.h>

/** 辨识速度点（轴端 rad/s，正负交替减小热漂移偏置）；单点 settle+sample 时长见下 */
#define FRICTION_IDENTIFY_POINTS              (12U)
#define FRICTION_IDENTIFY_SETTLE_TICKS        (1200U) /* 1.2s @1kHz */
#define FRICTION_IDENTIFY_SAMPLE_TICKS        (1000U) /* 1.0s @1kHz（均值散度减半） */
#define FRICTION_IDENTIFY_POINT_TIMEOUT_TICKS (3000U)
#define FRICTION_IDENTIFY_COMMAND_RETRIES     (100U)  /* 目标命令瞬态拒绝重试上限 @1kHz */
/** 结果门限：残差与对称偏差 ≤ 平均|力矩| 的该比例 */
/* 默认门限35%：实机残差≈25%为模型阶次系统性误差（Stribeck凹陷→负Kv）；
 * 数据质量由 asymmetry 与 Tc 复现性证明；垃圾数据残差>100%仍被拒。 */
#define FRICTION_IDENTIFY_TOLERANCE (0.35f)

static const float s_speed_schedule[FRICTION_IDENTIFY_POINTS] = {
    0.05f, -0.05f, 0.1f, -0.1f, 0.2f, -0.2f, 0.4f, -0.4f, 0.8f, -0.8f, 1.0f, -1.0f};

static struct {
    app_friction_identify_state_t state;
    app_friction_identify_failure_t failure;
    algo_friction_params_t result;
    float residual_rms_nm;
    float asymmetry_nm;
    uint32_t point_index;
    uint32_t tick;
    float sum_speed, sum_torque;
    uint32_t samples;
    float speeds[FRICTION_IDENTIFY_POINTS];
    float torques[FRICTION_IDENTIFY_POINTS];
    bool result_valid;
    bool command_pending;              /**< 速度点目标命令待落地（瞬态拒绝重试） */
    uint32_t command_retries;
    float tolerance;                   /**< 残差/对称门限（比例） */
    uint32_t elapsed_ticks;
    app_motion_reason_t motion_reason; /**< 撤销/拒绝时的 motion 原因 */
    uint32_t good_count, bad_count, hold_count;
} s_identify;

void app_friction_identify_init(void) { memset(&s_identify, 0, sizeof(s_identify)); }

bool app_friction_identify_busy(void) {
    return s_identify.state == APP_FRICTION_IDENTIFY_SETTLING
        || s_identify.state == APP_FRICTION_IDENTIFY_SAMPLING;
}

int app_friction_identify_start(float tolerance) {
    if (app_friction_identify_busy())
        return -1;
    if (tolerance == 0)
        tolerance = FRICTION_IDENTIFY_TOLERANCE;
    if (!(tolerance >= 0.05f && tolerance <= 1.0f))
        return -1;
    app_motion_snapshot_t motion;
    app_motion_get_snapshot(&motion);
    if (!motion.calibrated || motion.calibrating)
        return -2;
    if (motion.active)
        return -3;
    if (app_foc_get_state() != APP_FOC_STATE_READY && app_foc_get_state() != APP_FOC_STATE_RUN)
        return -4;
    memset(&s_identify, 0, sizeof(s_identify));
    s_identify.tolerance = tolerance;
    app_motion_friction_suspend(true); /* 辨识期挂起前馈：PI全权，起步无冲击 */
    s_identify.command_pending = true; /* 首个目标由 run_once 落地（带重试） */
    s_identify.state = APP_FRICTION_IDENTIFY_SETTLING;
    return 0;
}

void app_friction_identify_abort(void) {
    if (app_friction_identify_busy()) {
        (void)app_motion_stop();
        app_motion_friction_suspend(false);
        s_identify.state = APP_FRICTION_IDENTIFY_IDLE;
    }
}

/** @brief 采集 motion 原因与反馈健康到快照（完成/失败统一观测口径） */
static void identify_capture_health(void) {
    app_motion_snapshot_t motion;
    app_motion_get_snapshot(&motion);
    s_identify.motion_reason = motion.reason;
    s_identify.good_count = motion.good_count;
    s_identify.bad_count = motion.bad_count;
    s_identify.hold_count = motion.hold_count;
}

/** @brief 失败出口：撤销运动输出（失败结果不得 apply/save） */
static void identify_fail(app_friction_identify_failure_t failure) {
    identify_capture_health();         /* 撤销前采集诊断（定位瞬时判死路径） */
    (void)app_motion_release_output(); /* 释放输出（幂等），FOC 保持（用户要求） */
    app_motion_friction_suspend(false);
    s_identify.failure = failure;
    s_identify.result_valid = false;
    s_identify.state = APP_FRICTION_IDENTIFY_FAILED;
}

/** @brief 采样完成：拟合 + 残差/对称性门限，通过才置 DONE */
static void identify_fit(void) {
    algo_friction_params_t result = {
        .smoothing_rad_s =
            app_software_params_current()->control.speed_loop.friction_smoothing_rad_s};
    float residual = 0;
    float mean_abs = 0;
    float asymmetry = 0;

    if (algo_friction_fit(
            &result, &residual, s_identify.speeds, s_identify.torques, FRICTION_IDENTIFY_POINTS)
        != 0) {
        identify_fail(APP_FRICTION_IDENTIFY_FAIL_FIT);
        return;
    }
    for (uint32_t i = 0; i < FRICTION_IDENTIFY_POINTS; ++i)
        mean_abs += fabsf(s_identify.torques[i]);
    mean_abs /= FRICTION_IDENTIFY_POINTS;
    /* 正负配对（调度表交替排布）：奇对称摩擦 τ(+ω)+τ(−ω)≈0 */
    for (uint32_t i = 0; i < FRICTION_IDENTIFY_POINTS; i += 2)
        asymmetry += fabsf(s_identify.torques[i] + s_identify.torques[i + 1]);
    asymmetry /= FRICTION_IDENTIFY_POINTS / 2;
    /* 完成/失败均采集反馈健康（DONE 此前显示全 0） */
    identify_capture_health();
    /* 结果先入快照：门限拒绝也保留拟合值供检视（不得 apply） */
    s_identify.result = result;
    s_identify.residual_rms_nm = residual;
    s_identify.asymmetry_nm = asymmetry;
    if (!(mean_abs > 0) || residual > s_identify.tolerance * mean_abs) {
        identify_fail(APP_FRICTION_IDENTIFY_FAIL_RESIDUAL);
        return;
    }
    if (asymmetry > s_identify.tolerance * mean_abs) {
        identify_fail(APP_FRICTION_IDENTIFY_FAIL_ASYMMETRY);
        return;
    }
    s_identify.result_valid = true;
    s_identify.state = APP_FRICTION_IDENTIFY_DONE;
    app_motion_friction_suspend(false);
    (void)app_motion_release_output(); /* 释放输出，FOC 保持（用户要求） */
}

void app_friction_identify_run_once(void) {
    if (!app_friction_identify_busy())
        return;
    s_identify.elapsed_ticks++;
    if (s_identify.command_pending) {
        /* 目标命令可被瞬态坏拍保持拒绝：重试而非判死 */
        if (app_motion_speed(s_speed_schedule[s_identify.point_index]) == 0)
            s_identify.command_pending = false;
        else if (++s_identify.command_retries > FRICTION_IDENTIFY_COMMAND_RETRIES)
            identify_fail(APP_FRICTION_IDENTIFY_FAIL_COMMAND);
        return;
    }
    app_motion_snapshot_t motion;
    app_motion_get_snapshot(&motion);
    /* 单拍坏反馈由 motion 有界保持自愈，不判死；仅运动真正撤销才算失效 */
    if (!motion.active) {
        identify_fail(APP_FRICTION_IDENTIFY_FAIL_MOTION);
        return;
    }
    if (++s_identify.tick > FRICTION_IDENTIFY_POINT_TIMEOUT_TICKS) {
        identify_fail(APP_FRICTION_IDENTIFY_FAIL_NOT_SETTLED);
        return;
    }
    if (s_identify.state == APP_FRICTION_IDENTIFY_SETTLING) {
        if (s_identify.tick >= FRICTION_IDENTIFY_SETTLE_TICKS) {
            s_identify.state = APP_FRICTION_IDENTIFY_SAMPLING;
            s_identify.tick = 0;
            s_identify.sum_speed = 0;
            s_identify.sum_torque = 0;
            s_identify.samples = 0;
        }
        return;
    }
    s_identify.sum_speed += motion.speed_rad_s;
    s_identify.sum_torque += motion.torque_nm;
    if (++s_identify.samples < FRICTION_IDENTIFY_SAMPLE_TICKS)
        return;
    float mean_speed = s_identify.sum_speed / s_identify.samples;
    float mean_torque = s_identify.sum_torque / s_identify.samples;
    float target = s_speed_schedule[s_identify.point_index];

    if (fabsf(mean_speed - target) > 0.15f * fabsf(target) + 0.03f) {
        identify_fail(APP_FRICTION_IDENTIFY_FAIL_NOT_SETTLED);
        return;
    }
    s_identify.speeds[s_identify.point_index] = mean_speed;
    s_identify.torques[s_identify.point_index] = mean_torque;
    if (++s_identify.point_index >= FRICTION_IDENTIFY_POINTS) {
        identify_fit();
        return;
    }
    s_identify.tick = 0;
    s_identify.state = APP_FRICTION_IDENTIFY_SETTLING;
    s_identify.command_pending = true;
    s_identify.command_retries = 0;
}

void app_friction_identify_get_snapshot(app_friction_identify_snapshot_t* out) {
    if (out != NULL) {
        *out = (app_friction_identify_snapshot_t){
            .state = s_identify.state,
            .failure = s_identify.failure,
            .result = s_identify.result,
            .residual_rms_nm = s_identify.residual_rms_nm,
            .asymmetry_nm = s_identify.asymmetry_nm,
            .point_index = s_identify.point_index,
            .progress =
                ((float)s_identify.point_index
                 + (float)s_identify.tick
                       / (float)(FRICTION_IDENTIFY_SETTLE_TICKS + FRICTION_IDENTIFY_SAMPLE_TICKS))
                / (float)FRICTION_IDENTIFY_POINTS,
            .elapsed_ms = s_identify.elapsed_ticks,
            .command_retries = s_identify.command_retries,
            .motion_reason = s_identify.motion_reason,
            .good_count = s_identify.good_count,
            .bad_count = s_identify.bad_count,
            .hold_count = s_identify.hold_count,
            .result_valid = s_identify.result_valid,
        };
    }
}

int app_friction_identify_apply(void) {
    if (s_identify.state != APP_FRICTION_IDENTIFY_DONE || !s_identify.result_valid)
        return -1;
    const app_software_params_t* software = app_software_params_current();
    int rc = app_motion_gains(
        software->control.speed_loop.kp, software->control.speed_loop.ki,
        s_identify.result.coulomb_nm, s_identify.result.viscous_nm_per_rad_s);
    return rc == 0 ? 0 : -2; /* -2 = motion 正在驱动 */
}

int app_friction_identify_save(void) {
    return app_foc_get_state() == APP_FOC_STATE_OFF ? app_software_params_store() : -1;
}
