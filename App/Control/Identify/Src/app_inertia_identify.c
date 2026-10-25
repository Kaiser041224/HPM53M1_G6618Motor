/**
 * @file    app_inertia_identify.c
 * @brief   多电流/多速度、正反转双向惯量辨识状态机
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_inertia_identify.h"

#include "app_current_identify.h"
#include "app_foc.h"
#include "app_foc_current.h"
#include "app_friction_identify.h"
#include "app_motion.h"
#include "app_motor_identify.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "foc_math.h"

#include <math.h>
#include <string.h>

#define INERTIA_DEFAULT_MAX_CURRENT_A (2.0f)
#define INERTIA_CURRENT_HARD_MAX_A    (2.0f)
#define INERTIA_SETTLE_TIMEOUT        (2000U)
#define INERTIA_ZERO_SPEED_RAD_S      (0.05f)
#define INERTIA_MAX_SPEED_RAD_S       (3.0f)
#define INERTIA_POS_WINDOW_TICKS      (12U) /* 12ms 正/反转驱动窗口 */
#define INERTIA_NEG_WINDOW_TICKS      (24U) /* 制动窗口最大24ms */
#define INERTIA_NEG_MIN_TICKS         (5U)
#define INERTIA_NEG_SETTLE_TICKS      (1U)
#define INERTIA_ACCEL_TIMEOUT         (2000U)
#define INERTIA_MAX_TOTAL_J           (20.0f)
#define INERTIA_MAX_LOAD_J            (20.0f)
#define INERTIA_MIN_VALID_RUNS        (6U)  /* 至少3个点位 × 正反转 */
#define INERTIA_MIN_VALID_PER_DIR     (3U)  /* 正、反每个方向至少3次 */
#define INERTIA_MAX_MAD_RATIO         (0.15f)

/** @brief 点位：电流 + 起始/截断速度（轴端 rad/s） */
typedef struct {
    float current_a;
    float start_speed_rad_s;
    float stop_speed_rad_s;
} inertia_point_cfg_t;

static const inertia_point_cfg_t s_inertia_points[APP_INERTIA_IDENTIFY_MAX_POINTS] = {
    {0.60f, 0.50f, 0.30f},
    {0.80f, 0.60f, 0.35f},
    {1.00f, 0.80f, 0.40f},
    {1.20f, 0.90f, 0.45f},
    {1.50f, 1.00f, 0.50f},
    {2.00f, 1.00f, 0.50f},
};

static struct {
    app_inertia_identify_state_t state;
    app_inertia_identify_failure_t failure;
    float max_current_a;
    float point_current_a;
    float point_start_speed_rad_s;
    float point_stop_speed_rad_s;
    float torque_nm;
    float rotor_inertia_kgm2;
    float reducer_ratio;
    float kt_nm_per_a;
    float speed_peak_rad_s;
    float alpha_drive_rad_s2;
    float alpha_brake_rad_s2;
    float window[INERTIA_NEG_WINDOW_TICKS];
    float run_j_total[APP_INERTIA_IDENTIFY_MAX_RUNS];
    bool run_valid[APP_INERTIA_IDENTIFY_MAX_RUNS];
    uint16_t run_index;
    uint16_t run_count;
    uint16_t valid_runs;
    uint16_t valid_forward_runs;
    uint16_t valid_reverse_runs;
    uint16_t window_count;
    uint32_t tick;
    uint32_t elapsed_ms;
    uint32_t zero_ticks;
    uint32_t feedback_wait;
    int8_t direction;
    bool result_valid;
    bool applied;
    float median_j_total_kgm2;
    float mad_j_total_kgm2;
    float spread_ratio;
    algo_inertia_result_t result;
} s_identify;

/** @brief 最小二乘速度斜率 [rad/s²] */
static float inertia_slope(const float* sample, uint16_t count, float dt_s) {
    double sum_t = 0.0;
    double sum_y = 0.0;
    double sum_tt = 0.0;
    double sum_ty = 0.0;
    double n = (double)count;

    if ((sample == NULL) || (count < 2U) || !(dt_s > 0.0f)) {
        return 0.0f;
    }
    for (uint16_t i = 0U; i < count; i++) {
        double t = (double)i * (double)dt_s;
        double y = (double)sample[i];

        sum_t += t;
        sum_y += y;
        sum_tt += t * t;
        sum_ty += t * y;
    }
    double denominator = n * sum_tt - sum_t * sum_t;
    if (!(denominator > 0.0)) {
        return 0.0f;
    }
    return (float)((n * sum_ty - sum_t * sum_y) / denominator);
}

static int8_t inertia_sign(float speed) {
    if (speed > 0.0f) {
        return 1;
    }
    if (speed < 0.0f) {
        return -1;
    }
    return 0;
}

/** @brief 进度：完成采样数 + 当前相位 */
static float inertia_progress(void) {
    if ((s_identify.state == APP_INERTIA_IDENTIFY_DONE)
        || (s_identify.state == APP_INERTIA_IDENTIFY_FAILED)) {
        return 1.0f;
    }
    if (s_identify.run_count == 0U) {
        return 0.0f;
    }
    float phase = 0.0f;
    switch (s_identify.state) {
    case APP_INERTIA_IDENTIFY_SETTLE: phase = 0.05f; break;
    case APP_INERTIA_IDENTIFY_ACCEL: phase = 0.20f; break;
    case APP_INERTIA_IDENTIFY_MEASURE_POS: phase = 0.40f; break;
    case APP_INERTIA_IDENTIFY_NEG_SETTLE: phase = 0.50f; break;
    case APP_INERTIA_IDENTIFY_MEASURE_NEG: phase = 0.70f; break;
    default: phase = 0.0f; break;
    }
    return ((float)s_identify.run_index + phase) / (float)s_identify.run_count;
}

static void inertia_fail(app_inertia_identify_failure_t failure) {
    (void)app_foc_set_iq_ref(0.0f);
    (void)app_foc_set_id_ref(0.0f);
    s_identify.failure = failure;
    s_identify.result_valid = false;
    s_identify.state = APP_INERTIA_IDENTIFY_FAILED;
}

/** @brief 多点/双方向综合 */
static void inertia_aggregate(void) {
    float median = 0.0f;
    float mad = 0.0f;
    float spread = 0.0f;
    uint16_t valid = 0U;

    if (algo_inertia_aggregate(
            s_identify.run_j_total, s_identify.run_valid, s_identify.run_count,
            s_identify.rotor_inertia_kgm2, s_identify.reducer_ratio, &s_identify.result,
            &median, &mad, &spread, &valid)
        != 0) {
        inertia_fail(APP_INERTIA_IDENTIFY_FAIL_QUALITY);
        return;
    }
    uint16_t valid_forward = 0U;
    uint16_t valid_reverse = 0U;

    s_identify.valid_runs = valid;
    for (uint16_t i = 0U; i < s_identify.run_count; i++) {
        if (s_identify.run_valid[i]) {
            if ((i & 1U) == 0U) {
                valid_forward++;
            } else {
                valid_reverse++;
            }
        }
    }
    s_identify.valid_forward_runs = valid_forward;
    s_identify.valid_reverse_runs = valid_reverse;
    s_identify.median_j_total_kgm2 = median;
    s_identify.mad_j_total_kgm2 = mad;
    s_identify.spread_ratio = spread;
    if ((valid < INERTIA_MIN_VALID_RUNS) || (valid_forward < INERTIA_MIN_VALID_PER_DIR)
        || (valid_reverse < INERTIA_MIN_VALID_PER_DIR) || !(median > 0.0f)
        || ((mad / median) > INERTIA_MAX_MAD_RATIO)
        || (s_identify.result.j_total_kgm2 > INERTIA_MAX_TOTAL_J)
        || (s_identify.result.j_load_kgm2 > INERTIA_MAX_LOAD_J)) {
        inertia_fail(APP_INERTIA_IDENTIFY_FAIL_QUALITY);
        return;
    }
    s_identify.failure = APP_INERTIA_IDENTIFY_FAIL_NONE;
    s_identify.result_valid = true;
    s_identify.state = APP_INERTIA_IDENTIFY_DONE;
}

/** @brief 初始化一个采样：点索引 + 正/反方向 */
static void inertia_begin_run(uint16_t run_index) {
    uint16_t point_index = run_index / 2U;
    const inertia_point_cfg_t* point = &s_inertia_points[point_index];

    s_identify.run_index = run_index;
    s_identify.direction = ((run_index & 1U) == 0U) ? 1 : -1;
    s_identify.point_current_a = point->current_a;
    if (s_identify.point_current_a > s_identify.max_current_a) {
        s_identify.point_current_a = s_identify.max_current_a;
    }
    s_identify.point_start_speed_rad_s = point->start_speed_rad_s;
    s_identify.point_stop_speed_rad_s = point->stop_speed_rad_s;
    s_identify.torque_nm = s_identify.reducer_ratio * s_identify.kt_nm_per_a
                         * s_identify.point_current_a;
    s_identify.window_count = 0U;
    s_identify.tick = 0U;
    s_identify.zero_ticks = 0U;
    s_identify.alpha_drive_rad_s2 = 0.0f;
    s_identify.alpha_brake_rad_s2 = 0.0f;
    s_identify.state = APP_INERTIA_IDENTIFY_SETTLE;
}

/** @brief 一次采样结束：记录并进入下一点/下一方向 */
static void inertia_advance_run(bool run_ok) {
    (void)app_foc_set_iq_ref(0.0f);
    (void)app_foc_set_id_ref(0.0f);
    if (s_identify.run_index < s_identify.run_count) {
        s_identify.run_valid[s_identify.run_index] = run_ok;
        if (run_ok) {
            s_identify.valid_runs++;
        }
        s_identify.run_index++;
    }
    if (s_identify.run_index >= s_identify.run_count) {
        inertia_aggregate();
        return;
    }
    inertia_begin_run(s_identify.run_index);
}

/** @brief 单次采样局部失败：作废该次，继续下一方向/点位 */
static void inertia_run_fail(app_inertia_identify_failure_t failure) {
    s_identify.failure = failure;
    inertia_advance_run(false);
}

void app_inertia_identify_init(void) { memset(&s_identify, 0, sizeof(s_identify)); }

bool app_inertia_identify_busy(void) {
    return (s_identify.state != APP_INERTIA_IDENTIFY_IDLE)
        && (s_identify.state != APP_INERTIA_IDENTIFY_DONE)
        && (s_identify.state != APP_INERTIA_IDENTIFY_FAILED);
}

int app_inertia_identify_start(float max_current_a) {
    const app_motor_params_t* motor = app_motor_params_current();
    const app_software_params_t* software = app_software_params_current();
    app_motion_snapshot_t motion;
    app_foc_state_t foc_state = app_foc_get_state();

    if (app_inertia_identify_busy()) {
        return -1;
    }
    if ((foc_state != APP_FOC_STATE_READY) && (foc_state != APP_FOC_STATE_RUN)) {
        return -2;
    }
    if (app_foc_stop_pending()) {
        return -2;
    }
    app_motion_get_snapshot(&motion);
    if (motion.active || motion.calibrating) {
        return -3;
    }
    if (app_motor_identify_is_active() || app_current_identify_is_active()
        || app_friction_identify_busy() || app_foc_current_vtest_active()) {
        return -4;
    }
    if (!foc_finite(motor->kt_nm_per_a) || (motor->kt_nm_per_a <= 0.0f)
        || !foc_finite(motor->reducer_ratio) || (motor->reducer_ratio <= 0.0f)
        || !foc_finite(motor->inertia_kgm2) || (motor->inertia_kgm2 < 0.0f)) {
        return -1;
    }

    float limit = software->control.limits.i_q_max_a;
    if (!foc_finite(limit) || (limit <= 0.0f)) {
        return -1;
    }
    if (!foc_finite(max_current_a) || (max_current_a <= 0.0f)) {
        max_current_a = INERTIA_DEFAULT_MAX_CURRENT_A;
    }
    if (max_current_a > INERTIA_CURRENT_HARD_MAX_A) {
        max_current_a = INERTIA_CURRENT_HARD_MAX_A;
    }
    if (max_current_a > limit) {
        max_current_a = limit;
    }
    if (max_current_a <= 0.0f) {
        return -1;
    }

    if (app_foc_get_current_source() != APP_FOC_CURRENT_SOURCE_DIRECT) {
        if ((foc_state != APP_FOC_STATE_READY)
            || (app_foc_set_current_source(APP_FOC_CURRENT_SOURCE_DIRECT) != 0)) {
            return -5;
        }
    }
    if ((app_foc_set_iq_ref(0.0f) != 0) || (app_foc_set_id_ref(0.0f) != 0)) {
        return -5;
    }

    memset(&s_identify, 0, sizeof(s_identify));
    s_identify.max_current_a = max_current_a;
    s_identify.rotor_inertia_kgm2 = motor->inertia_kgm2;
    s_identify.reducer_ratio = motor->reducer_ratio;
    s_identify.kt_nm_per_a = motor->kt_nm_per_a;
    s_identify.run_count = APP_INERTIA_IDENTIFY_MAX_RUNS;
    s_identify.run_index = 0U;
    inertia_begin_run(0U);
    if (!foc_finite(s_identify.torque_nm) || (s_identify.torque_nm <= 0.0f)) {
        inertia_fail(APP_INERTIA_IDENTIFY_FAIL_CALC);
        return -1;
    }
    return 0;
}

void app_inertia_identify_abort(void) {
    if (!app_inertia_identify_busy()) {
        return;
    }
    (void)app_foc_set_iq_ref(0.0f);
    (void)app_foc_set_id_ref(0.0f);
    s_identify.failure = APP_INERTIA_IDENTIFY_FAIL_ABORTED;
    s_identify.result_valid = false;
    s_identify.state = APP_INERTIA_IDENTIFY_IDLE;
}

void app_inertia_identify_run_once(void) {
    if (!app_inertia_identify_busy()) {
        return;
    }
    s_identify.elapsed_ms++;
    if (app_foc_stop_pending()
        || ((app_foc_get_state() != APP_FOC_STATE_READY)
            && (app_foc_get_state() != APP_FOC_STATE_RUN))
        || (app_foc_get_current_source() != APP_FOC_CURRENT_SOURCE_DIRECT)) {
        inertia_fail(APP_INERTIA_IDENTIFY_FAIL_STATE);
        return;
    }
    app_motion_snapshot_t motion;
    app_motion_get_snapshot(&motion);
    if (motion.active) {
        inertia_fail(APP_INERTIA_IDENTIFY_FAIL_MOTION);
        return;
    }
    app_foc_current_snapshot_t foc;
    app_foc_get_snapshot(&foc);
    if (!foc.speed_out_valid || !foc_finite(foc.speed_out_rad_s)) {
        if (++s_identify.feedback_wait > 50U) {
            inertia_fail(APP_INERTIA_IDENTIFY_FAIL_FEEDBACK);
        }
        return;
    }
    s_identify.feedback_wait = 0U;
    float speed = foc.speed_out_rad_s;
    float speed_abs = fabsf(speed);
    if (speed_abs > s_identify.speed_peak_rad_s) {
        s_identify.speed_peak_rad_s = speed_abs;
    }

    switch (s_identify.state) {
    case APP_INERTIA_IDENTIFY_SETTLE:
        if (speed_abs > INERTIA_ZERO_SPEED_RAD_S) {
            s_identify.zero_ticks = 0U;
            if (++s_identify.tick > INERTIA_SETTLE_TIMEOUT) {
                inertia_run_fail(APP_INERTIA_IDENTIFY_FAIL_TIMEOUT);
            }
            return;
        }
        if (++s_identify.zero_ticks < 5U) {
            return;
        }
        if (app_foc_set_iq_ref((float)s_identify.direction * s_identify.point_current_a)
            != 0) {
            inertia_run_fail(APP_INERTIA_IDENTIFY_FAIL_STATE);
            return;
        }
        s_identify.tick = 0U;
        s_identify.state = APP_INERTIA_IDENTIFY_ACCEL;
        return;

    case APP_INERTIA_IDENTIFY_ACCEL:
        if ((inertia_sign(speed) != s_identify.direction)
            && (speed_abs > INERTIA_ZERO_SPEED_RAD_S)) {
            inertia_run_fail(APP_INERTIA_IDENTIFY_FAIL_WINDOW);
            return;
        }
        if (speed_abs > INERTIA_MAX_SPEED_RAD_S) {
            inertia_run_fail(APP_INERTIA_IDENTIFY_FAIL_SPEED);
            return;
        }
        if ((speed_abs >= s_identify.point_start_speed_rad_s)
            && (inertia_sign(speed) == s_identify.direction)) {
            s_identify.window_count = 0U;
            s_identify.state = APP_INERTIA_IDENTIFY_MEASURE_POS;
            return;
        }
        if (++s_identify.tick > INERTIA_ACCEL_TIMEOUT) {
            inertia_run_fail(APP_INERTIA_IDENTIFY_FAIL_TIMEOUT);
        }
        return;

    case APP_INERTIA_IDENTIFY_MEASURE_POS:
        if ((inertia_sign(speed) != s_identify.direction)
            || (speed_abs > INERTIA_MAX_SPEED_RAD_S)) {
            inertia_run_fail(APP_INERTIA_IDENTIFY_FAIL_WINDOW);
            return;
        }
        s_identify.window[s_identify.window_count++] = speed;
        if (s_identify.window_count < INERTIA_POS_WINDOW_TICKS) {
            return;
        }
        s_identify.alpha_drive_rad_s2 = inertia_slope(
            s_identify.window, s_identify.window_count, 0.001f);
        if (app_foc_set_iq_ref(-(float)s_identify.direction * s_identify.point_current_a)
            != 0) {
            inertia_run_fail(APP_INERTIA_IDENTIFY_FAIL_STATE);
            return;
        }
        s_identify.window_count = 0U;
        s_identify.tick = 0U;
        s_identify.state = APP_INERTIA_IDENTIFY_NEG_SETTLE;
        return;

    case APP_INERTIA_IDENTIFY_NEG_SETTLE:
        if ((inertia_sign(speed) != s_identify.direction)
            || (speed_abs < s_identify.point_stop_speed_rad_s)) {
            inertia_run_fail(APP_INERTIA_IDENTIFY_FAIL_WINDOW);
            return;
        }
        if (++s_identify.tick < INERTIA_NEG_SETTLE_TICKS) {
            return;
        }
        s_identify.window_count = 0U;
        s_identify.state = APP_INERTIA_IDENTIFY_MEASURE_NEG;
        return;

    case APP_INERTIA_IDENTIFY_MEASURE_NEG:
        if ((inertia_sign(speed) != s_identify.direction)
            || (speed_abs > INERTIA_MAX_SPEED_RAD_S)) {
            inertia_run_fail(APP_INERTIA_IDENTIFY_FAIL_WINDOW);
            return;
        }
        s_identify.window[s_identify.window_count++] = speed;
        if ((speed_abs > s_identify.point_stop_speed_rad_s)
            && (s_identify.window_count < INERTIA_NEG_WINDOW_TICKS)) {
            return;
        }
        if (s_identify.window_count < INERTIA_NEG_MIN_TICKS) {
            inertia_run_fail(APP_INERTIA_IDENTIFY_FAIL_WINDOW);
            return;
        }
        s_identify.alpha_brake_rad_s2 = inertia_slope(
            s_identify.window, s_identify.window_count, 0.001f);
        (void)app_foc_set_iq_ref(0.0f);
        {
            /* 把 drive/brake 两段斜率折算到“电流正方向”坐标后再统一公式。 */
            float alpha_drive = s_identify.alpha_drive_rad_s2 * (float)s_identify.direction;
            float alpha_brake = s_identify.alpha_brake_rad_s2 * (float)s_identify.direction;
            algo_inertia_result_t run_result;

            if ((algo_inertia_calculate(
                     s_identify.torque_nm, alpha_drive, alpha_brake,
                     s_identify.rotor_inertia_kgm2, s_identify.reducer_ratio, &run_result)
                 != 0)
                || (run_result.j_total_kgm2 > INERTIA_MAX_TOTAL_J)
                || (run_result.j_load_kgm2 > INERTIA_MAX_LOAD_J)) {
                inertia_run_fail(APP_INERTIA_IDENTIFY_FAIL_CALC);
                return;
            }
            s_identify.run_j_total[s_identify.run_index] = run_result.j_total_kgm2;
        }
        inertia_advance_run(true);
        return;

    default:
        return;
    }
}

void app_inertia_identify_get_snapshot(app_inertia_identify_snapshot_t* out) {
    if (out == NULL) {
        return;
    }
    uint16_t mask = 0U;

    for (uint16_t i = 0U; i < APP_INERTIA_IDENTIFY_MAX_RUNS; i++) {
        if (s_identify.run_valid[i]) {
            mask |= (uint16_t)(1U << i);
        }
    }
    *out = (app_inertia_identify_snapshot_t){
        .state = s_identify.state,
        .failure = s_identify.failure,
        .busy = app_inertia_identify_busy(),
        .result_valid = s_identify.result_valid,
        .applied = s_identify.applied,
        .result = s_identify.result,
        .current_a = s_identify.point_current_a,
        .torque_nm = s_identify.torque_nm,
        .alpha_pos_rad_s2 = s_identify.alpha_drive_rad_s2,
        .alpha_neg_rad_s2 = s_identify.alpha_brake_rad_s2,
        .speed_peak_rad_s = s_identify.speed_peak_rad_s,
        .elapsed_ms = s_identify.elapsed_ms,
        .max_current_a = s_identify.max_current_a,
        .run_index = s_identify.run_index,
        .run_count = s_identify.run_count,
        .point_count = APP_INERTIA_IDENTIFY_MAX_POINTS,
        .valid_runs = s_identify.valid_runs,
        .valid_forward_runs = s_identify.valid_forward_runs,
        .valid_reverse_runs = s_identify.valid_reverse_runs,
        .progress = inertia_progress(),
        .median_j_total_kgm2 = s_identify.median_j_total_kgm2,
        .mad_j_total_kgm2 = s_identify.mad_j_total_kgm2,
        .spread_ratio = s_identify.spread_ratio,
    };
    for (uint16_t i = 0U; i < APP_INERTIA_IDENTIFY_MAX_RUNS; i++) {
        uint16_t point = i / 2U;

        out->run_j_total[i] = s_identify.run_j_total[i];
        out->run_current[i] = s_inertia_points[point].current_a;
        if (out->run_current[i] > s_identify.max_current_a) {
            out->run_current[i] = s_identify.max_current_a;
        }
        out->run_start_speed[i] = s_inertia_points[point].start_speed_rad_s;
        out->run_stop_speed[i] = s_inertia_points[point].stop_speed_rad_s;
        out->run_direction[i] = ((i & 1U) == 0U) ? 1 : -1;
    }
    out->run_valid_mask = mask;
}

int app_inertia_identify_apply(void) {
    if (!s_identify.result_valid || (s_identify.state != APP_INERTIA_IDENTIFY_DONE)) {
        return -1;
    }
    if (app_foc_get_state() != APP_FOC_STATE_OFF) {
        return -2;
    }
    app_motor_params_mutable()->load_inertia_kgm2 = s_identify.result.j_load_kgm2;
    s_identify.applied = true;
    return 0;
}

int app_inertia_identify_save(void) {
    if (!s_identify.applied || (app_foc_get_state() != APP_FOC_STATE_OFF)) {
        return -1;
    }
    return app_motor_params_store();
}
