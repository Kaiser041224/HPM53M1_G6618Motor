/** @file app_motion.c
 * @brief motion实时状态唯一写者，静态游标相位标定
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_motion.h"
#include "algo_motion.h"
#include "app_encoder.h"
#include "app_foc.h"
#include "app_motor_params.h"
#include "app_param.h"
#include "app_ramfunc.h"
#include "foc_math.h"
#include "intf_clock.h"
#include "intf_sys.h"
static app_motion_snapshot_t s_motion;
volatile float g_motion_shaft_angle_rad __attribute__((section(".noncacheable.bss")));
volatile float g_motion_shaft_angle_deg __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_motion_shaft_angle_valid __attribute__((section(".noncacheable.bss")));
static algo_motion_pi_t s_velocity_pi;
static float s_target, s_velocity_kp, s_velocity_ki;
static float s_previous_angle, s_zero_rad, s_previous_rotor;
static float s_calibration_sum, s_calibration_first;
static float s_calibration_phase;
static uint32_t s_previous_cycle, s_previous_sequence, s_epoch, s_request_sequence;
static uint32_t s_previous_sample_cycle;                /**< 上一个用于测速的配对快照时间 */
static float s_seconds_per_cycle;
static bool s_primed, s_speed_mode;
static float s_active_direction, s_active_kt;
static float s_calibrated_direction;
static const app_motor_params_t* s_motion_motor_params; /**< init解析，仅读RAM参数对象 */
/** @brief 最新有效反馈不超过500us */
static bool motion_feedback_fresh(void) {
    return s_motion.feedback_valid
        && (float)(uint32_t)(intf_clock_get_cycle() - s_previous_cycle) * s_seconds_per_cycle
               <= .0005f;
}
typedef struct {
    uint32_t version;
    float phase, zero, direction;
    uint32_t zero_valid;
} motion_record_t;
_Static_assert(sizeof(motion_record_t) <= APP_PARAM_DATA_MAX, "MOTN record exceeds parameter slot");
/** @brief 差值归一化到+-半圈 */
APP_ATTR_RAMFUNC
static float motion_delta(float value) {
    return value > .5f ? value - 1.0f : value < -.5f ? value + 1.0f : value;
}
/** @brief 清理motion自身输出观测，不代表FOC缓降已经完成 */
APP_ATTR_RAMFUNC
static void motion_clear_output(app_motion_reason_t reason) {
    s_motion.active = false;
    s_velocity_pi.integral = 0;
    s_motion.torque_nm = 0;
    s_motion.iq_a = 0;
    s_motion.reason = reason;
}
/** @brief 反馈失效撤销活动状态并提交零请求，不自动恢复 */
APP_ATTR_RAMFUNC
static void motion_invalid(app_motion_reason_t reason) {
    s_motion.feedback_valid = false;
    s_motion.calibrating = false;
    g_motion_shaft_angle_valid = 0;
    s_motion.bad_count++;
    if (s_motion.active) {
        app_foc_current_request_t request = {0, 0, ++s_request_sequence, s_epoch, true};
        (void)app_foc_submit_current(&request);
    }
    motion_clear_output(reason);
}
void app_motion_init(void) {
    s_motion = (app_motion_snapshot_t){0};
    g_motion_shaft_angle_rad = 0;
    g_motion_shaft_angle_deg = 0;
    g_motion_shaft_angle_valid = 0;
    s_primed = false;
    s_previous_sequence = 0;
    s_previous_cycle = 0;
    s_previous_sample_cycle = 0;
    s_zero_rad = 0;
    s_calibrated_direction = 0;
    s_motion_motor_params = app_motor_params_current();
    s_seconds_per_cycle = 1.0f / intf_clock_get_cpu_freq();
    s_velocity_pi = (algo_motion_pi_t){0};
    s_velocity_kp = 0;
    s_velocity_ki = 0;
    motion_record_t record;
    if (app_param_load(APP_PARAM_KEY_MOTION, &record, sizeof(record)) == 0 && record.version == 1
        && foc_finite(record.phase) && record.phase >= 0 && record.phase < .02f
        && foc_finite(record.zero) && fabsf(record.zero) < FOC_TWO_PI_F && record.zero_valid <= 1
        && (record.direction == 1 || record.direction == -1)
        && record.direction == s_motion_motor_params->encoder.direction) {
        s_motion.phase_turns = record.phase;
        s_motion.calibrated = true;
        s_calibrated_direction = record.direction;
        s_zero_rad = record.zero;
        s_motion.zero_valid = record.zero_valid != 0;
    }
}
int app_motion_calibrate(void) {
    uint32_t st = intf_sys_irq_save();
    if (app_foc_get_state() != APP_FOC_STATE_OFF || s_motion.active) {
        intf_sys_irq_restore(st);
        return -1;
    }
    s_motion.calibrating = true;
    s_motion.feedback_valid = false;
    g_motion_shaft_angle_valid = 0;
    s_motion.calibration_samples = 0;
    s_motion.calibration_done = false;
    s_motion.reason = APP_MOTION_REASON_NONE;
    s_calibration_sum = 0;
    s_primed = false;
    intf_sys_irq_restore(st);
    return 0;
}
int app_motion_apply(void) {
    uint32_t st = intf_sys_irq_save();
    if (app_foc_get_state() != APP_FOC_STATE_OFF || !s_motion.calibration_done
        || s_motion.calibrating) {
        intf_sys_irq_restore(st);
        return -1;
    }
    float direction = app_motor_params_current()->encoder.direction;
    if (direction != 1 && direction != -1) {
        intf_sys_irq_restore(st);
        return -1;
    }
    s_motion.phase_turns = s_calibration_phase;
    s_calibrated_direction = direction;
    s_motion.calibrated = true;
    s_motion.calibration_done = false;
    s_motion.zero_valid = false;
    s_primed = false;
    s_motion.feedback_valid = false;
    g_motion_shaft_angle_valid = 0;
    intf_sys_irq_restore(st);
    return 0;
}
int app_motion_zero(void) {
    uint32_t st = intf_sys_irq_save();
    if (app_foc_get_state() != APP_FOC_STATE_OFF || !s_motion.calibrated || !motion_feedback_fresh()
        || s_calibrated_direction != app_motor_params_current()->encoder.direction
        || s_motion.calibrating) {
        intf_sys_irq_restore(st);
        return -1;
    }
    s_zero_rad = s_previous_angle;
    s_motion.zero_valid = true;
    s_motion.shaft_rad = 0;
    g_motion_shaft_angle_rad = 0;
    g_motion_shaft_angle_deg = 0;
    g_motion_shaft_angle_valid = 1;
    intf_sys_irq_restore(st);
    return 0;
}
int app_motion_save(void) {
    motion_record_t record;
    uint32_t st = intf_sys_irq_save();
    if (app_foc_get_state() != APP_FOC_STATE_OFF || !s_motion.calibrated || s_motion.calibrating
        || s_calibrated_direction != app_motor_params_current()->encoder.direction) {
        intf_sys_irq_restore(st);
        return -1;
    }
    record = (motion_record_t){
        1, s_motion.phase_turns, s_zero_rad, s_calibrated_direction, s_motion.zero_valid};
    intf_sys_irq_restore(st);
    return app_param_store(APP_PARAM_KEY_MOTION, &record, sizeof(record));
}
int app_motion_gains(float kp, float ki) {
    if (!foc_finite(kp) || !foc_finite(ki) || kp < 0 || ki < 0 || kp > 100 || ki > 1000)
        return -1;
    uint32_t st = intf_sys_irq_save();
    if (app_foc_get_state() != APP_FOC_STATE_OFF) {
        intf_sys_irq_restore(st);
        return -1;
    }
    s_velocity_kp = kp;
    s_velocity_ki = ki;
    intf_sys_irq_restore(st);
    return 0;
}
/** @brief 命令发布，与ISR短临界区互斥 */
static int motion_command(float target, bool speed) {
    if (!foc_finite(target))
        return -1;
    uint32_t st = intf_sys_irq_save();
    float kt = app_motor_params_current()->kt_nm_per_a;
    if (!foc_finite(kt) || kt <= 0 || !s_motion.calibrated || !motion_feedback_fresh()
        || s_calibrated_direction != app_motor_params_current()->encoder.direction
        || s_motion.calibrating || (speed && (s_velocity_kp <= 0 || fabsf(target) > .5f))
        || (!speed && fabsf(target) > 50 * kt)) {
        intf_sys_irq_restore(st);
        return -1;
    }
    if (s_motion.active
        && (s_active_kt != kt
            || s_active_direction != app_motor_params_current()->encoder.direction)) {
        intf_sys_irq_restore(st);
        return -1;
    }
    if (app_foc_stop_pending()
        || (app_foc_get_state() != APP_FOC_STATE_READY
            && app_foc_get_state() != APP_FOC_STATE_RUN)) {
        intf_sys_irq_restore(st);
        return -1;
    }
    if (!s_motion.active) {
        if (app_foc_set_current_source(APP_FOC_CURRENT_SOURCE_MOTION) != 0) {
            intf_sys_irq_restore(st);
            return -1;
        }
        s_epoch = app_foc_current_epoch();
    }
    if (s_motion.active && s_speed_mode != speed) {
        intf_sys_irq_restore(st);
        return -1;
    }
    s_speed_mode = speed;
    s_target = target;
    s_motion.active = true;
    s_motion.reason = APP_MOTION_REASON_NONE;
    s_active_direction = app_motor_params_current()->encoder.direction;
    s_active_kt = kt;
    intf_sys_irq_restore(st);
    return 0;
}
int app_motion_torque(float torque) { return motion_command(torque, false); }
int app_motion_speed(float speed) { return motion_command(speed, true); }
int app_motion_stop(void) {
    uint32_t st = intf_sys_irq_save();
    motion_clear_output(APP_MOTION_REASON_USER_STOP);
    intf_sys_irq_restore(st);
    return app_foc_request_stop();
}
APP_ATTR_RAMFUNC
void app_motion_step(void) {
    app_encoder_rotor_snapshot_t rotor;
    uint16_t raw;
    uint32_t now = intf_clock_get_cycle();
    if (app_encoder_output_sample(&rotor, &raw) != 0) {
        s_motion.sample_failure_count++;
        motion_invalid(APP_MOTION_REASON_SAMPLE);
        return;
    }
    if (rotor.seq == s_previous_sequence) {
        s_motion.duplicate_count++;
        motion_invalid(APP_MOTION_REASON_DUPLICATE);
        return;
    }
    s_previous_sequence = rotor.seq;
    if (!foc_finite(rotor.rad) || rotor.rad < 0 || rotor.rad >= FOC_TWO_PI_F) {
        motion_invalid(APP_MOTION_REASON_SAMPLE);
        return;
    }
    float u = rotor.rad / FOC_TWO_PI_F, v = (float)raw / 65536.0f;
    if (s_motion.calibrating) {
        if (app_foc_get_state() != APP_FOC_STATE_OFF) {
            s_motion.calibrating = false;
            s_motion.reason = APP_MOTION_REASON_FOC_STATE;
            return;
        }
        float scaled_phase = (v - .98f * u) * 50.0f;
        int32_t phase_whole = (int32_t)scaled_phase;
        if ((float)phase_whole > scaled_phase)
            phase_whole--;
        float phase = (scaled_phase - (float)phase_whole) * .02f;
        if (s_motion.calibration_samples == 0) {
            s_calibration_first = phase;
            s_previous_rotor = u;
        }
        float difference = motion_delta((phase - s_calibration_first) * 50) / 50;
        float movement = motion_delta(u - s_previous_rotor);
        if (movement > .001f || movement < -.001f || difference > .002f || difference < -.002f) {
            s_motion.calibrating = false;
            s_motion.reason = APP_MOTION_REASON_CALIBRATION;
            return;
        }
        s_calibration_sum += difference;
        if (++s_motion.calibration_samples == 1000) {
            float mean = s_calibration_first + s_calibration_sum / 1000;
            if (mean < 0)
                mean += .02f;
            else if (mean >= .02f)
                mean -= .02f;
            s_calibration_phase = mean;
            s_motion.calibration_done = true;
            s_motion.calibrating = false;
            s_primed = false;
        }
        return;
    }
    float angle;
    if (!s_motion.calibrated) {
        s_motion.feedback_valid = false;
        g_motion_shaft_angle_valid = 0;
        return;
    }
    if (algo_motion_vernier(u, v, s_motion.phase_turns, &angle) != 0) {
        s_motion.vernier_failure_count++;
        motion_invalid(APP_MOTION_REASON_VERNIER);
        return;
    }
    float direction = s_motion_motor_params->encoder.direction;
    if ((direction != 1 && direction != -1) || direction != s_calibrated_direction) {
        motion_invalid(APP_MOTION_REASON_PARAMETER);
        return;
    }
    angle *= direction;
    float dt = (float)(now - s_previous_cycle) * s_seconds_per_cycle;
    float sample_dt =
        (float)(uint32_t)(rotor.timestamp_cycles - s_previous_sample_cycle) * s_seconds_per_cycle;
    if (s_motion.active && (dt <= 0 || dt > .0002f)) {
        s_motion.timing_failure_count++;
        motion_invalid(APP_MOTION_REASON_TIMING);
        return;
    }
    if (s_primed && sample_dt > 0 && sample_dt < .001f) {
        float velocity = foc_wrap_pm_pi(angle - s_previous_angle) / sample_dt;
        s_motion.speed_rad_s += .03f * (velocity - s_motion.speed_rad_s);
    } else
        s_motion.speed_rad_s = 0;
    s_previous_angle = angle;
    s_previous_cycle = now;
    s_previous_sample_cycle = rotor.timestamp_cycles;
    s_primed = true;
    s_motion.shaft_rad = foc_wrap_2pi(angle - (s_motion.zero_valid ? s_zero_rad : 0));
    g_motion_shaft_angle_rad = s_motion.shaft_rad;
    g_motion_shaft_angle_deg = s_motion.shaft_rad * (360.0f / FOC_TWO_PI_F);
    g_motion_shaft_angle_valid = 1;
    s_motion.feedback_valid = true;
    s_motion.good_count++;
    if (!s_motion.active)
        return;
    if (s_epoch != app_foc_current_epoch() || app_foc_stop_pending()
        || (app_foc_get_state() != APP_FOC_STATE_READY
            && app_foc_get_state() != APP_FOC_STATE_RUN)) {
        motion_clear_output(APP_MOTION_REASON_FOC_STATE);
        return;
    }
    float kt = s_motion_motor_params->kt_nm_per_a;
    if (!foc_finite(kt) || kt <= 0 || kt != s_active_kt || direction != s_active_direction) {
        motion_invalid(APP_MOTION_REASON_PARAMETER);
        return;
    }
    float torque = s_speed_mode ? algo_motion_speed(
                                      &s_velocity_pi, s_target, s_motion.speed_rad_s, s_velocity_kp,
                                      s_velocity_ki, dt, 50 * kt)
                                : s_target;
    s_motion.torque_nm = torque;
    s_motion.iq_a = torque / (50 * kt);
    app_foc_current_request_t request = {0, s_motion.iq_a, ++s_request_sequence, s_epoch, true};
    if (app_foc_submit_current(&request) != 0)
        motion_clear_output(APP_MOTION_REASON_SUBMIT);
}
void app_motion_get_snapshot(app_motion_snapshot_t* out) {
    if (out) {
        uint32_t st = intf_sys_irq_save();
        *out = s_motion;
        intf_sys_irq_restore(st);
    }
}
