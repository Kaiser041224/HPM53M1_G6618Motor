/**
 * @file    app_cogging_identify.c
 * @brief   齿槽/传动角表低速正反扫自整定
 * @author  Kaiser
 *
 * 标定对象：电机本体 + 减速器，不装外部负载。
 * 采样：1kHz，motion 速度环实际 q 电流；正向/反向各若干输出圈。
 * 提取：同一角度分箱的 (I_fwd + I_rev)/2 抵消恒定摩擦，输出角表再乘减速比。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_cogging_identify.h"

#include "app_current_identify.h"
#include "app_encoder.h"
#include "app_foc.h"
#include "app_foc_current.h"
#include "app_friction_identify.h"
#include "app_inertia_identify.h"
#include "app_motor_identify.h"
#include "app_motion.h"
#include "app_motor_params.h"
#include "foc_math.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define COGGING_TWO_PI             (6.283185307179586f)
#define COGGING_SETTLE_TIMEOUT_MS  (2000U)
#define COGGING_STOP_TIMEOUT_MS    (2000U)
#define COGGING_MAX_SPEED_RAD_S    (5.0f)
#define COGGING_MAX_REVS           (10U)
#define COGGING_SETTLE_FACTOR      (0.60f)
#define COGGING_SWEEP_FACTOR       (0.50f)
#define COGGING_STOP_SPEED_RAD_S   (0.08f)

typedef struct {
    uint32_t fwd_samples;
    uint32_t rev_samples;
    float fwd_sum_a;
    float fwd_sq_sum_a2;
    float rev_sum_a;
    float rev_sq_sum_a2;
} cogging_point_acc_t;

static struct {
    app_cogging_identify_state_t state;
    app_cogging_identify_failure_t failure;
    float speed_cmd_rad_s;
    float speed_rad_s;
    float kt_nm_per_a;
    float reducer_ratio;
    float travel_rad;
    float prev_shaft_rad;
    uint32_t elapsed_ms;
    uint32_t state_ms;
    uint32_t samples;
    uint16_t revs;
    uint8_t point_index;
    uint8_t point_count;
    float point_speeds[APP_COGGING_IDENTIFY_MAX_POINTS];
    uint16_t point_revs[APP_COGGING_IDENTIFY_MAX_POINTS];
    cogging_point_acc_t point_acc[APP_COGGING_IDENTIFY_MAX_POINTS];
    int8_t direction;
    int8_t first_direction;
    app_cogging_tables_t result;
    bool result_valid;
} s_identify;

static algo_cogging_bin_t s_rotor_bins[ALGO_COGGING_ROTOR_POINTS];
static algo_cogging_bin_t s_output_bins[ALGO_COGGING_OUTPUT_POINTS];

static bool cogging_finite(float value) { return isfinite(value); }

static int8_t cogging_sign(float value) {
    if (value > 0.0f) {
        return 1;
    }
    if (value < 0.0f) {
        return -1;
    }
    return 0;
}

static uint16_t cogging_bin(float angle_rad, uint16_t points) {
    float wrapped = fmodf(angle_rad, COGGING_TWO_PI);
    if (wrapped < 0.0f) {
        wrapped += COGGING_TWO_PI;
    }
    uint16_t bin = (uint16_t)(wrapped * (float)points / COGGING_TWO_PI);
    return (bin < points) ? bin : (uint16_t)(points - 1U);
}

static void cogging_fail(app_cogging_identify_failure_t failure) {
    (void)app_motion_release_output();
    app_motion_set_output_tracking(false);
    app_cogging_suspend(false);
    s_identify.failure = failure;
    s_identify.state = APP_COGGING_IDENTIFY_FAILED;
}

static void cogging_finalize(void) {
    s_identify.state = APP_COGGING_IDENTIFY_FINALIZE;
    memset(&s_identify.result, 0, sizeof(s_identify.result));

    int rotor_rc = algo_cogging_finalize_bins(
        s_rotor_bins, ALGO_COGGING_ROTOR_POINTS, s_identify.kt_nm_per_a,
        s_identify.reducer_ratio, false, APP_COGGING_ROTOR_SCALE_NM,
        s_identify.result.rotor, &s_identify.result.rotor_stats);
    int output_rc = algo_cogging_finalize_bins(
        s_output_bins, ALGO_COGGING_OUTPUT_POINTS, s_identify.kt_nm_per_a,
        s_identify.reducer_ratio, true, APP_COGGING_OUTPUT_SCALE_NM,
        s_identify.result.output, &s_identify.result.output_stats);

    s_identify.result.rotor_valid =
        (rotor_rc == 0)
        && (s_identify.result.rotor_stats.valid_bins >= (ALGO_COGGING_ROTOR_POINTS / 2U));
    s_identify.result.output_valid =
        (output_rc == 0)
        && (s_identify.result.output_stats.valid_bins >= (ALGO_COGGING_OUTPUT_POINTS / 2U));
    s_identify.result_valid = s_identify.result.rotor_valid || s_identify.result.output_valid;

    app_motion_set_output_tracking(false);
    app_cogging_suspend(false);
    if (s_identify.result_valid) {
        s_identify.failure = APP_COGGING_IDENTIFY_FAIL_NONE;
        s_identify.state = APP_COGGING_IDENTIFY_DONE;
    } else {
        s_identify.failure = APP_COGGING_IDENTIFY_FAIL_QUALITY;
        s_identify.state = APP_COGGING_IDENTIFY_FAILED;
    }
}

void app_cogging_identify_init(void) {
    memset(&s_identify, 0, sizeof(s_identify));
    memset(s_rotor_bins, 0, sizeof(s_rotor_bins));
    memset(s_output_bins, 0, sizeof(s_output_bins));
    s_identify.state = APP_COGGING_IDENTIFY_IDLE;
}

static void cogging_point_begin(uint8_t index) {
    s_identify.point_index = index;
    s_identify.speed_cmd_rad_s = s_identify.point_speeds[index];
    s_identify.revs = s_identify.point_revs[index];
    s_identify.travel_rad = 0.0f;
    s_identify.state_ms = 0U;
    s_identify.direction = 0;
    s_identify.first_direction = 0;
    memset(&s_identify.point_acc[index], 0, sizeof(s_identify.point_acc[index]));
}

bool app_cogging_identify_busy(void) {
    return (s_identify.state != APP_COGGING_IDENTIFY_IDLE)
        && (s_identify.state != APP_COGGING_IDENTIFY_DONE)
        && (s_identify.state != APP_COGGING_IDENTIFY_FAILED);
}

int app_cogging_identify_start(float speed_rad_s, uint16_t revs) {
    const app_motor_params_t* motor = app_motor_params_current();
    app_motion_snapshot_t motion;

    if (app_cogging_identify_busy()) {
        return -1;
    }
    if ((app_foc_get_state() != APP_FOC_STATE_READY)
        && (app_foc_get_state() != APP_FOC_STATE_RUN)) {
        return -2;
    }
    if (app_foc_stop_pending() || app_foc_current_vtest_active()) {
        return -2;
    }
    app_motion_get_snapshot(&motion);
    if (!motion.calibrated || motion.calibrating || motion.active) {
        return -3;
    }
    if (app_motor_identify_is_active() || app_current_identify_is_active()
        || app_friction_identify_busy() || app_inertia_identify_busy()) {
        return -4;
    }
    float speed_schedule[APP_COGGING_IDENTIFY_MAX_POINTS];
    uint16_t revs_schedule[APP_COGGING_IDENTIFY_MAX_POINTS];
    uint8_t point_count;

    /* speed>0：单点快速检查；speed<=0：默认多点 0.5/1.0/2.0/3.0 rad/s。
     * 低速点圈时长、采样多，等效加权低速段（齿槽在低速最可见）；
     * 高速点用于观察速度依赖与摩擦不对称。每点正反各 revs 圈。 */
    if (!cogging_finite(speed_rad_s) || (speed_rad_s <= 0.0f)) {
        point_count = 4U;
        speed_schedule[0] = 0.5f;
        speed_schedule[1] = 1.0f;
        speed_schedule[2] = 2.0f;
        speed_schedule[3] = 3.0f;
        uint16_t point_revs = (revs == 0U) ? APP_COGGING_IDENTIFY_MULTI_REVS : revs;
        if (point_revs > COGGING_MAX_REVS) {
            point_revs = COGGING_MAX_REVS;
        }
        revs_schedule[0] = point_revs;
        revs_schedule[1] = point_revs;
        revs_schedule[2] = point_revs;
        revs_schedule[3] = point_revs;
    } else {
        point_count = 1U;
        speed_schedule[0] = (speed_rad_s > COGGING_MAX_SPEED_RAD_S)
                                ? COGGING_MAX_SPEED_RAD_S
                                : speed_rad_s;
        uint16_t point_revs = (revs == 0U) ? APP_COGGING_IDENTIFY_DEFAULT_REVS : revs;
        if (point_revs > COGGING_MAX_REVS) {
            point_revs = COGGING_MAX_REVS;
        }
        revs_schedule[0] = point_revs;
    }
    if (!cogging_finite(motor->kt_nm_per_a) || (motor->kt_nm_per_a <= 0.0f)
        || !cogging_finite(motor->reducer_ratio) || (motor->reducer_ratio <= 0.0f)) {
        return -1;
    }

    memset(&s_identify, 0, sizeof(s_identify));
    memset(s_rotor_bins, 0, sizeof(s_rotor_bins));
    memset(s_output_bins, 0, sizeof(s_output_bins));
    s_identify.kt_nm_per_a = motor->kt_nm_per_a;
    s_identify.reducer_ratio = motor->reducer_ratio;
    s_identify.point_count = point_count;
    for (uint8_t i = 0U; i < point_count; i++) {
        s_identify.point_speeds[i] = speed_schedule[i];
        s_identify.point_revs[i] = revs_schedule[i];
    }
    cogging_point_begin(0U);
    s_identify.state = APP_COGGING_IDENTIFY_SETTLE_FWD;
    s_identify.failure = APP_COGGING_IDENTIFY_FAIL_NONE;
    app_cogging_suspend(true);
    app_motion_set_output_tracking(true);
    if (app_motion_speed(s_identify.speed_cmd_rad_s) != 0) {
        app_motion_set_output_tracking(false);
        app_cogging_suspend(false);
        s_identify.failure = APP_COGGING_IDENTIFY_FAIL_MOTION;
        s_identify.state = APP_COGGING_IDENTIFY_FAILED;
        return -5;
    }
    return 0;
}

void app_cogging_identify_abort(void) {
    if (!app_cogging_identify_busy()) {
        return;
    }
    (void)app_motion_release_output();
    app_motion_set_output_tracking(false);
    app_cogging_suspend(false);
    s_identify.failure = APP_COGGING_IDENTIFY_FAIL_ABORTED;
    s_identify.state = APP_COGGING_IDENTIFY_FAILED;
}

static void cogging_sample(float rotor_rad, float output_rad, float iq_a, int8_t direction) {
    uint16_t rotor_bin = cogging_bin(rotor_rad, ALGO_COGGING_ROTOR_POINTS);
    uint16_t output_bin = cogging_bin(output_rad, ALGO_COGGING_OUTPUT_POINTS);
    algo_cogging_bin_add(&s_rotor_bins[rotor_bin], iq_a, direction);
    algo_cogging_bin_add(&s_output_bins[output_bin], iq_a, direction);

    cogging_point_acc_t* point = &s_identify.point_acc[s_identify.point_index];
    if (direction > 0) {
        point->fwd_sum_a += iq_a;
        point->fwd_sq_sum_a2 += iq_a * iq_a;
        point->fwd_samples++;
    } else if (direction < 0) {
        point->rev_sum_a += iq_a;
        point->rev_sq_sum_a2 += iq_a * iq_a;
        point->rev_samples++;
    }
    s_identify.samples++;
}

static bool cogging_wait_settle(float speed, float target, int8_t expected_sign) {
    if (cogging_sign(speed) == expected_sign && fabsf(speed) >= target * COGGING_SETTLE_FACTOR) {
        return true;
    }
    return false;
}

void app_cogging_identify_run_once(void) {
    app_motion_snapshot_t motion;
    app_encoder_rotor_snapshot_t rotor;
    app_foc_current_snapshot_t foc;

    if (!app_cogging_identify_busy()) {
        return;
    }
    s_identify.elapsed_ms++;
    s_identify.state_ms++;

    if (app_foc_stop_pending()
        || ((app_foc_get_state() != APP_FOC_STATE_READY)
            && (app_foc_get_state() != APP_FOC_STATE_RUN))) {
        cogging_fail(APP_COGGING_IDENTIFY_FAIL_STATE);
        return;
    }
    app_motion_get_snapshot(&motion);
    if (!motion.active || !motion.feedback_valid) {
        cogging_fail(APP_COGGING_IDENTIFY_FAIL_MOTION);
        return;
    }
    if (app_encoder_get_rotor_snapshot(&rotor) != 0 || !rotor.valid) {
        cogging_fail(APP_COGGING_IDENTIFY_FAIL_FEEDBACK);
        return;
    }
    app_foc_get_snapshot(&foc);
    if (!cogging_finite(foc.i_q_a) || !cogging_finite(motion.speed_rad_s)
        || !cogging_finite(motion.shaft_rad) || !cogging_finite(rotor.rad)) {
        cogging_fail(APP_COGGING_IDENTIFY_FAIL_SAMPLE);
        return;
    }
    /* FOC 在 READY/零给定拍可能 valid=0；只有真正进入采样扫掠后才要求电流环有效。 */
    if (((s_identify.state == APP_COGGING_IDENTIFY_SWEEP_FWD)
         || (s_identify.state == APP_COGGING_IDENTIFY_SWEEP_REV))
        && !foc.valid) {
        cogging_fail(APP_COGGING_IDENTIFY_FAIL_FEEDBACK);
        return;
    }
    s_identify.speed_rad_s = motion.speed_rad_s;

    switch (s_identify.state) {
    case APP_COGGING_IDENTIFY_SETTLE_FWD:
        if (s_identify.state_ms > COGGING_SETTLE_TIMEOUT_MS) {
            cogging_fail(APP_COGGING_IDENTIFY_FAIL_TIMEOUT);
            return;
        }
        if (cogging_wait_settle(motion.speed_rad_s, s_identify.speed_cmd_rad_s, 1)
            || cogging_wait_settle(motion.speed_rad_s, s_identify.speed_cmd_rad_s, -1)) {
            s_identify.direction = cogging_sign(motion.speed_rad_s);
            s_identify.prev_shaft_rad = motion.shaft_rad;
            s_identify.travel_rad = 0.0f;
            s_identify.state = APP_COGGING_IDENTIFY_SWEEP_FWD;
            s_identify.state_ms = 0U;
        }
        return;

    case APP_COGGING_IDENTIFY_SWEEP_FWD:
    case APP_COGGING_IDENTIFY_SWEEP_REV: {
        if ((cogging_sign(motion.speed_rad_s) != s_identify.direction)
            || (fabsf(motion.speed_rad_s) < (s_identify.speed_cmd_rad_s * COGGING_SWEEP_FACTOR))) {
            cogging_fail(APP_COGGING_IDENTIFY_FAIL_SAMPLE);
            return;
        }
        float delta = foc_wrap_pm_pi(motion.shaft_rad - s_identify.prev_shaft_rad);
        s_identify.prev_shaft_rad = motion.shaft_rad;
        s_identify.travel_rad += fabsf(delta);
        /* 分箱用物理输出角（未应用零点/方向）：与运行期注入的索引基准一致
         * （app_motion_step 传 raw 角给 app_cogging），re-zero 不失效表相位。 */
        cogging_sample(rotor.rad, motion.shaft_raw_rad, foc.i_q_a, s_identify.direction);
        if (s_identify.travel_rad >= (COGGING_TWO_PI * (float)s_identify.revs)) {
            (void)app_motion_speed(0.0f);
            s_identify.state_ms = 0U;
            s_identify.state = (s_identify.state == APP_COGGING_IDENTIFY_SWEEP_FWD)
                                   ? APP_COGGING_IDENTIFY_STOP_FWD
                                   : APP_COGGING_IDENTIFY_STOP_REV;
        }
        /* 单次扫掠超时：按当前速度点圈数给 2.5× 名义时间 + 2s 启动余量。 */
        float sweep_s = COGGING_TWO_PI * (float)s_identify.revs / s_identify.speed_cmd_rad_s;
        uint32_t sweep_timeout_ms = (uint32_t)(2500.0f * sweep_s) + 2000U;
        if (s_identify.state_ms > sweep_timeout_ms) {
            cogging_fail(APP_COGGING_IDENTIFY_FAIL_TIMEOUT);
        }
        return;
    }

    case APP_COGGING_IDENTIFY_STOP_FWD:
    case APP_COGGING_IDENTIFY_STOP_REV:
        if (s_identify.state_ms > COGGING_STOP_TIMEOUT_MS) {
            cogging_fail(APP_COGGING_IDENTIFY_FAIL_TIMEOUT);
            return;
        }
        if (fabsf(motion.speed_rad_s) > COGGING_STOP_SPEED_RAD_S) {
            return;
        }
        if (s_identify.state == APP_COGGING_IDENTIFY_STOP_FWD) {
            s_identify.first_direction = s_identify.direction;
            s_identify.state = APP_COGGING_IDENTIFY_SETTLE_REV;
            s_identify.state_ms = 0U;
            s_identify.travel_rad = 0.0f;
            if (app_motion_speed(-s_identify.speed_cmd_rad_s) != 0) {
                cogging_fail(APP_COGGING_IDENTIFY_FAIL_MOTION);
            }
        } else if ((uint8_t)(s_identify.point_index + 1U) < s_identify.point_count) {
            cogging_point_begin((uint8_t)(s_identify.point_index + 1U));
            s_identify.state = APP_COGGING_IDENTIFY_SETTLE_FWD;
            if (app_motion_speed(s_identify.speed_cmd_rad_s) != 0) {
                cogging_fail(APP_COGGING_IDENTIFY_FAIL_MOTION);
            }
        } else {
            (void)app_motion_release_output();
            cogging_finalize();
        }
        return;

    case APP_COGGING_IDENTIFY_SETTLE_REV:
        if (s_identify.state_ms > COGGING_SETTLE_TIMEOUT_MS) {
            cogging_fail(APP_COGGING_IDENTIFY_FAIL_TIMEOUT);
            return;
        }
        if (cogging_wait_settle(motion.speed_rad_s, s_identify.speed_cmd_rad_s, 1)
            || cogging_wait_settle(motion.speed_rad_s, s_identify.speed_cmd_rad_s, -1)) {
            int8_t direction = cogging_sign(motion.speed_rad_s);
            if (direction == s_identify.first_direction) {
                cogging_fail(APP_COGGING_IDENTIFY_FAIL_SAMPLE);
                return;
            }
            s_identify.direction = direction;
            s_identify.prev_shaft_rad = motion.shaft_rad;
            s_identify.travel_rad = 0.0f;
            s_identify.state = APP_COGGING_IDENTIFY_SWEEP_REV;
            s_identify.state_ms = 0U;
        }
        return;

    default:
        return;
    }
}

void app_cogging_identify_get_snapshot(app_cogging_identify_snapshot_t* out) {
    if (out == NULL) {
        return;
    }
    float rev_angle = COGGING_TWO_PI * (float)s_identify.revs;
    float travel_frac = (rev_angle > 0.0f)
                            ? fminf(s_identify.travel_rad / rev_angle, 1.0f)
                            : 0.0f;
    float point_frac = 0.0f;
    switch (s_identify.state) {
    case APP_COGGING_IDENTIFY_SETTLE_FWD: point_frac = 0.02f; break;
    case APP_COGGING_IDENTIFY_SWEEP_FWD: point_frac = 0.02f + 0.46f * travel_frac; break;
    case APP_COGGING_IDENTIFY_STOP_FWD: point_frac = 0.50f; break;
    case APP_COGGING_IDENTIFY_SETTLE_REV: point_frac = 0.52f; break;
    case APP_COGGING_IDENTIFY_SWEEP_REV: point_frac = 0.52f + 0.46f * travel_frac; break;
    case APP_COGGING_IDENTIFY_STOP_REV: point_frac = 0.98f; break;
    case APP_COGGING_IDENTIFY_FINALIZE: point_frac = 1.0f; break;
    case APP_COGGING_IDENTIFY_DONE:
    case APP_COGGING_IDENTIFY_FAILED: point_frac = 1.0f; break;
    default: point_frac = 0.0f; break;
    }
    float progress = 1.0f;
    if ((s_identify.state != APP_COGGING_IDENTIFY_DONE)
        && (s_identify.state != APP_COGGING_IDENTIFY_FAILED)
        && (s_identify.point_count > 0U)) {
        progress = ((float)s_identify.point_index + point_frac)
                   / (float)s_identify.point_count;
        if (progress > 1.0f) {
            progress = 1.0f;
        }
    }

    *out = (app_cogging_identify_snapshot_t){
        .state = s_identify.state,
        .failure = s_identify.failure,
        .busy = app_cogging_identify_busy(),
        .result_valid = s_identify.result_valid,
        .rotor_valid = s_identify.result.rotor_valid,
        .output_valid = s_identify.result.output_valid,
        .speed_cmd_rad_s = s_identify.speed_cmd_rad_s,
        .speed_rad_s = s_identify.speed_rad_s,
        .travel_rad = s_identify.travel_rad,
        .progress = progress,
        .elapsed_ms = s_identify.elapsed_ms,
        .samples = s_identify.samples,
        .revs = s_identify.revs,
        .point_index = s_identify.point_index,
        .point_count = s_identify.point_count,
        .rotor_stats = s_identify.result.rotor_stats,
        .output_stats = s_identify.result.output_stats,
    };
    for (uint8_t i = 0U; i < s_identify.point_count; i++) {
        const cogging_point_acc_t* acc = &s_identify.point_acc[i];
        app_cogging_identify_point_t* point = &out->points[i];
        point->speed_rad_s = s_identify.point_speeds[i];
        point->revs = s_identify.point_revs[i];
        point->fwd_samples = acc->fwd_samples;
        point->rev_samples = acc->rev_samples;
        if (acc->fwd_samples > 0U) {
            point->fwd_iq_mean_a = acc->fwd_sum_a / (float)acc->fwd_samples;
            point->fwd_iq_rms_a = sqrtf(acc->fwd_sq_sum_a2 / (float)acc->fwd_samples);
        }
        if (acc->rev_samples > 0U) {
            point->rev_iq_mean_a = acc->rev_sum_a / (float)acc->rev_samples;
            point->rev_iq_rms_a = sqrtf(acc->rev_sq_sum_a2 / (float)acc->rev_samples);
        }
    }
}

int app_cogging_identify_get_result(app_cogging_tables_t* out) {
    if ((out == NULL) || !s_identify.result_valid) {
        return -1;
    }
    *out = s_identify.result;
    return 0;
}

int app_cogging_identify_apply(void) {
    if (app_cogging_identify_busy() || !s_identify.result_valid
        || (app_foc_get_state() != APP_FOC_STATE_OFF) || app_foc_stop_pending()) {
        return -1;
    }
    if (app_cogging_set_tables(&s_identify.result) != 0) {
        return -1;
    }
    /* apply 后按 YAML 默认选择表和总开关；enable=false 时仍需手动 cogging on。 */
    app_cogging_apply_defaults();
    return 0;
}
