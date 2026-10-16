/** @file app_motion.c
 * @brief motion实时状态唯一写者，静态游标相位标定
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_motion.h"
#include "algo_friction.h"
#include "algo_motion.h"
#include "app_encoder.h"
#include "app_foc.h"
#include "app_motor_params.h"
#include "app_param.h"
#include "app_ramfunc.h"
#include "app_software_params.h"
#include "foc_math.h"
#include "intf_clock.h"
#include "intf_sys.h"
static app_motion_snapshot_t s_motion;
volatile float g_motion_shaft_angle_rad __attribute__((section(".noncacheable.bss")));
volatile float g_motion_shaft_angle_deg __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_motion_shaft_angle_valid __attribute__((section(".noncacheable.bss")));
static algo_motion_pi_t s_velocity_pi;
static float s_target, s_velocity_kp, s_velocity_ki;
static algo_friction_params_t s_friction_model; /**< 摩擦前馈模型（Tc/Kv/平滑；init/gains 消费） */
static bool s_friction_suspended;               /**< 辨识期挂起前馈（速度模式FF置零） */
static float s_velocity_lpf_coeff;              /**< 测速一阶滤波系数（参数 init 期消费） */
static float s_motion_iq_max_a;             /**< motion 命令电流硬限制[A]（力矩命令与PI输出上限） */
static float s_motion_speed_limit_rad_s;    /**< 轴端速度命令限[rad/s]（0=不限） */
static float s_previous_angle, s_zero_rad, s_previous_rotor;
static float s_calibration_sum, s_calibration_first;
static float s_calibration_phase;
static uint32_t s_previous_cycle, s_previous_sequence, s_epoch, s_request_sequence;
static uint32_t s_previous_sample_cycle;    /**< 上一个用于测速的配对快照时间 */
static float s_seconds_per_cycle;
static bool s_primed, s_speed_mode;
static float s_active_direction, s_active_kt, s_active_reducer;
static float s_calibrated_direction;
static uint32_t s_hold_samples;             /**< 当前连续坏拍数（active质量类保持窗口） */
static bool s_velocity_gap;                 /**< 保持恢复后首拍跳过跨隙测速差分 */
static float s_nearest_turn;                /**< 最近解析的游标整数圈号（兜底测速基准） */
static bool s_have_nearest;
static uint32_t s_vernier_fallback_samples; /**< 连续兜底样本数（超限判真失效） */
#define APP_MOTION_VERNIER_FALLBACK_MAX_SAMPLES (5000U) /**< 兜底上限500ms@10kHz */
static const app_motor_params_t* s_motion_motor_params; /**< init解析，仅读RAM参数对象 */
/* 保持窗口10kHz×200=20ms零力矩：游标残差坏拍簇（齿隙/扭振瞬态）可穿过；
 * 游标每拍独立重解绝对圈号，恢复无累计误差。>窗口判真失败。 */
#define APP_MOTION_HOLD_MAX_SAMPLES (200U)
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
    s_hold_samples = 0;
    s_velocity_gap = false;
}
/** @brief 硬撤销：反馈/参数失效终止活动并提交零请求，不自动恢复 */
APP_ATTR_RAMFUNC
static void motion_abort(app_motion_reason_t reason) {
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
/** @brief 反馈拒绝：active期间质量类坏拍在保持窗口内零力矩容忍，超限/参数类硬撤销 */
APP_ATTR_RAMFUNC
static void motion_reject(app_motion_reason_t reason) {
    bool holdable = reason == APP_MOTION_REASON_SAMPLE || reason == APP_MOTION_REASON_DUPLICATE
                 || reason == APP_MOTION_REASON_VERNIER || reason == APP_MOTION_REASON_TIMING;
    if (s_motion.active && holdable) {
        if (s_hold_samples == 0)
            s_motion.hold_count++;
        if (++s_hold_samples <= APP_MOTION_HOLD_MAX_SAMPLES) {
            /* 有界保持：零力矩、冻结积分（不调用PI）、保留mode/target */
            s_velocity_gap = true;
            s_motion.feedback_valid = false;
            g_motion_shaft_angle_valid = 0;
            s_motion.bad_count++;
            s_motion.reason = reason;
            s_motion.torque_nm = 0;
            s_motion.iq_a = 0;
            app_foc_current_request_t request = {0, 0, ++s_request_sequence, s_epoch, true};
            if (app_foc_submit_current(&request) != 0)
                motion_clear_output(APP_MOTION_REASON_SUBMIT);
            return;
        }
    }
    motion_abort(reason);
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
    s_hold_samples = 0;
    s_velocity_gap = false;
    s_have_nearest = false;
    s_nearest_turn = 0;
    s_vernier_fallback_samples = 0;
    s_zero_rad = 0;
    s_calibrated_direction = 0;
    s_motion_motor_params = app_motor_params_current();
    s_seconds_per_cycle = 1.0f / intf_clock_get_cpu_freq();
    s_velocity_pi = (algo_motion_pi_t){0};
    /* 力矩/速度环参数：YAML 出厂值 + flash 记忆覆盖（init 期消费；motion gains 在线改） */
    const app_software_params_t* software = app_software_params_current();
    s_velocity_kp = software->control.speed_loop.kp;
    s_velocity_ki = software->control.speed_loop.ki;
    s_friction_model.coulomb_nm = software->control.speed_loop.friction_coulomb_nm;
    s_friction_model.viscous_nm_per_rad_s =
        software->control.speed_loop.friction_viscous_nm_per_rad_s;
    s_friction_model.smoothing_rad_s = software->control.speed_loop.friction_smoothing_rad_s;
    if (!foc_finite(s_friction_model.coulomb_nm) || s_friction_model.coulomb_nm < 0)
        s_friction_model.coulomb_nm = 0;
    /* Kv 允许负值（Stribeck 斜率）；仅非有限/越界回退 0 */
    if (!foc_finite(s_friction_model.viscous_nm_per_rad_s)
        || s_friction_model.viscous_nm_per_rad_s < -100
        || s_friction_model.viscous_nm_per_rad_s > 100)
        s_friction_model.viscous_nm_per_rad_s = 0;
    if (!foc_finite(s_friction_model.smoothing_rad_s) || s_friction_model.smoothing_rad_s < 0)
        s_friction_model.smoothing_rad_s = 0;
    s_velocity_lpf_coeff = software->control.speed_loop.velocity_lpf_coeff;
    if (!foc_finite(s_velocity_lpf_coeff) || s_velocity_lpf_coeff <= 0 || s_velocity_lpf_coeff > 1)
        s_velocity_lpf_coeff = .03f;
    s_motion_iq_max_a = software->control.limits.motion_iq_max_a;
    if (!foc_finite(s_motion_iq_max_a) || s_motion_iq_max_a <= 0)
        s_motion_iq_max_a = 1.0f;
    s_motion_speed_limit_rad_s = software->control.limits.motion_speed_limit_rad_s;
    if (!foc_finite(s_motion_speed_limit_rad_s) || s_motion_speed_limit_rad_s < 0)
        s_motion_speed_limit_rad_s = 0;
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
    s_have_nearest = false; /* 相位重标定：旧圈号基准失效 */
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
    s_have_nearest = false; /* 新相位：旧圈号基准失效 */
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
int app_motion_release_output(void) {
    uint32_t st = intf_sys_irq_save();
    motion_clear_output(APP_MOTION_REASON_NONE);
    app_foc_current_request_t request = {0, 0, ++s_request_sequence, s_epoch, true};
    (void)app_foc_submit_current(&request);
    intf_sys_irq_restore(st);
    return 0;
}
int app_motion_gains(float kp, float ki, float coulomb_nm, float viscous_nm_per_rad_s) {
    /* Kv 允许负值：Stribeck 拟合斜率（摩擦随速度下降）是合法物理量 */
    if (!foc_finite(kp) || !foc_finite(ki) || !foc_finite(coulomb_nm)
        || !foc_finite(viscous_nm_per_rad_s) || kp < 0 || ki < 0 || coulomb_nm < 0
        || viscous_nm_per_rad_s < -100 || viscous_nm_per_rad_s > 100 || kp > 100 || ki > 1000
        || coulomb_nm > 100)
        return APP_MOTION_CMD_BAD_PARAM;
    uint32_t st = intf_sys_irq_save();
    if (s_motion.active) { /* 安全意图：运动驱动中不改环路增益 */
        intf_sys_irq_restore(st);
        return APP_MOTION_CMD_FOC_NOT_READY;
    }
    s_velocity_kp = kp;
    s_velocity_ki = ki;
    s_friction_model.coulomb_nm = coulomb_nm;
    s_friction_model.viscous_nm_per_rad_s = viscous_nm_per_rad_s;
    app_software_params_t* software = app_software_params_mutable();
    software->control.speed_loop.kp = kp;
    software->control.speed_loop.ki = ki;
    software->control.speed_loop.friction_coulomb_nm = coulomb_nm;
    software->control.speed_loop.friction_viscous_nm_per_rad_s = viscous_nm_per_rad_s;
    intf_sys_irq_restore(st);
    return 0;
}
/** @brief 命令发布，与ISR短临界区互斥；返回app_motion_cmd_status_t */
static int motion_command(float target, bool speed) {
    if (!foc_finite(target))
        return APP_MOTION_CMD_BAD_PARAM;
    uint32_t st = intf_sys_irq_save();
    float kt = s_motion_motor_params->kt_nm_per_a;
    float reducer = s_motion_motor_params->reducer_ratio;
    if (!foc_finite(kt) || kt <= 0 || !foc_finite(reducer) || reducer <= 0) {
        intf_sys_irq_restore(st);
        return APP_MOTION_CMD_BAD_PARAM;
    }
    if (!s_motion.calibrated || s_motion.calibrating
        || s_calibrated_direction != s_motion_motor_params->encoder.direction) {
        intf_sys_irq_restore(st);
        return APP_MOTION_CMD_NOT_CALIBRATED;
    }
    if (!motion_feedback_fresh()) {
        intf_sys_irq_restore(st);
        return APP_MOTION_CMD_STALE_FEEDBACK;
    }
    /* 速度命令限幅由参数 motion_speed_limit_rad_s 决定（0=不限）；力矩受 motion_iq_max_a 硬限制 */
    if ((!speed && fabsf(target) > reducer * kt * s_motion_iq_max_a)
        || (speed && s_motion_speed_limit_rad_s > 0
            && fabsf(target) > s_motion_speed_limit_rad_s)) {
        intf_sys_irq_restore(st);
        return APP_MOTION_CMD_TARGET_LIMIT;
    }
    if (speed && s_velocity_kp <= 0) {
        intf_sys_irq_restore(st);
        return APP_MOTION_CMD_GAIN_NOT_SET;
    }
    if (app_foc_stop_pending()
        || (app_foc_get_state() != APP_FOC_STATE_READY
            && app_foc_get_state() != APP_FOC_STATE_RUN)) {
        intf_sys_irq_restore(st);
        return APP_MOTION_CMD_FOC_NOT_READY;
    }
    if (s_motion.active && s_speed_mode != speed) {
        intf_sys_irq_restore(st);
        return APP_MOTION_CMD_MODE_LOCKED;
    }
    if (s_motion.active
        && (s_active_kt != kt || s_active_reducer != reducer
            || s_active_direction != s_motion_motor_params->encoder.direction)) {
        intf_sys_irq_restore(st);
        return APP_MOTION_CMD_BASE_CHANGED;
    }
    if (!s_motion.active) {
        /* 授权仍有效（source/epoch未变）时续用不是切换；失效才需重新申请 */
        if (app_foc_get_current_source() != APP_FOC_CURRENT_SOURCE_MOTION
            || app_foc_current_epoch() != s_epoch) {
            if (app_foc_set_current_source(APP_FOC_CURRENT_SOURCE_MOTION) != 0) {
                intf_sys_irq_restore(st);
                return APP_MOTION_CMD_OWNER_DENIED;
            }
            s_epoch = app_foc_current_epoch();
        }
    }
    s_speed_mode = speed;
    s_target = target;
    s_motion.active = true;
    s_motion.reason = APP_MOTION_REASON_NONE;
    s_active_direction = s_motion_motor_params->encoder.direction;
    s_active_kt = kt;
    s_active_reducer = reducer;
    intf_sys_irq_restore(st);
    return 0;
}
int app_motion_torque(float torque) { return motion_command(torque, false); }
int app_motion_speed(float speed) { return motion_command(speed, true); }
void app_motion_friction_suspend(bool suspend) {
    uint32_t st = intf_sys_irq_save();
    s_friction_suspended = suspend;
    intf_sys_irq_restore(st);
}

int app_motion_stop(void) {
    uint32_t st = intf_sys_irq_save();
    motion_clear_output(APP_MOTION_REASON_USER_STOP);
    intf_sys_irq_restore(st);
    return app_foc_request_stop() == 0 ? 0 : APP_MOTION_CMD_OWNER_DENIED;
}
APP_ATTR_RAMFUNC
void app_motion_step(void) {
    app_encoder_rotor_snapshot_t rotor;
    uint16_t raw;
    uint32_t now = intf_clock_get_cycle();
    if (app_encoder_output_sample(&rotor, &raw) != 0) {
        s_motion.sample_failure_count++;
        motion_reject(APP_MOTION_REASON_SAMPLE);
        return;
    }
    if (rotor.seq == s_previous_sequence) {
        s_motion.duplicate_count++;
        motion_reject(APP_MOTION_REASON_DUPLICATE);
        return;
    }
    s_previous_sequence = rotor.seq;
    if (!foc_finite(rotor.rad) || rotor.rad < 0 || rotor.rad >= FOC_TWO_PI_F) {
        motion_reject(APP_MOTION_REASON_SAMPLE);
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
    if (algo_motion_vernier(u, v, s_motion.phase_turns, &angle) == 0) {
        s_vernier_fallback_samples = 0;
        s_nearest_turn = angle * (50.0f / FOC_TWO_PI_F) - u;
        s_have_nearest = true;
    } else {
        /* 游标失配（齿隙/扭振瞬态或低力矩松垂）：转子角+最近圈号兜底测速。
         * 失配偏移≤齿隙（输出轴≤0.15°），对速度环无感；零力矩保持会在松垂区
         * 自我强化拒帧导致停死，故兜底优先。持续超限才判真失效。 */
        s_motion.vernier_failure_count++;
        if (!s_motion.calibrated || !s_have_nearest
            || ++s_vernier_fallback_samples > APP_MOTION_VERNIER_FALLBACK_MAX_SAMPLES) {
            motion_reject(APP_MOTION_REASON_VERNIER);
            return;
        }
        s_motion.vernier_fallback_count++;
        angle = (s_nearest_turn + u) * (FOC_TWO_PI_F / 50.0f);
    }
    float direction = s_motion_motor_params->encoder.direction;
    if ((direction != 1 && direction != -1) || direction != s_calibrated_direction) {
        motion_reject(APP_MOTION_REASON_PARAMETER);
        return;
    }
    angle *= direction;
    float dt = (float)(now - s_previous_cycle) * s_seconds_per_cycle;
    float sample_dt =
        (float)(uint32_t)(rotor.timestamp_cycles - s_previous_sample_cycle) * s_seconds_per_cycle;
    /* 恢复拍（保持窗口后首拍）允许跨隙dt，之后恢复200us入口纪律 */
    if (s_motion.active && s_hold_samples == 0 && (dt <= 0 || dt > .0002f)) {
        s_motion.timing_failure_count++;
        motion_reject(APP_MOTION_REASON_TIMING);
        return;
    }
    if (s_velocity_gap) {
        s_velocity_gap = false; /* 跨保持间隙的差分不进入测速滤波 */
    } else if (s_primed && sample_dt > 0 && sample_dt < .001f) {
        float velocity = foc_wrap_pm_pi(angle - s_previous_angle) / sample_dt;
        s_motion.speed_rad_s += s_velocity_lpf_coeff * (velocity - s_motion.speed_rad_s);
    } else if (!s_primed) {
        s_motion.speed_rad_s = 0;
    }
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
    s_hold_samples = 0;
    if (!s_motion.active)
        return;
    if (s_epoch != app_foc_current_epoch() || app_foc_stop_pending()
        || (app_foc_get_state() != APP_FOC_STATE_READY
            && app_foc_get_state() != APP_FOC_STATE_RUN)) {
        motion_clear_output(APP_MOTION_REASON_FOC_STATE);
        return;
    }
    float kt = s_motion_motor_params->kt_nm_per_a;
    float reducer = s_motion_motor_params->reducer_ratio;
    if (!foc_finite(kt) || kt <= 0 || !foc_finite(reducer) || reducer <= 0 || kt != s_active_kt
        || reducer != s_active_reducer || direction != s_active_direction) {
        motion_reject(APP_MOTION_REASON_PARAMETER);
        return;
    }
    /* FOC执行反馈：仅采信属于本请求的确认执行；限幅/过期/未确认→冻结速度PI积分 */
    app_foc_execution_t execution;
    app_foc_get_execution(&execution);
    bool execution_ours = execution.valid && execution.epoch == s_epoch
                       && execution.request_sequence == s_request_sequence;
    s_motion.iq_accepted_a = execution_ours ? execution.accepted_iq_a : 0;
    s_motion.current_limited = execution_ours && execution.current_limited;
    s_motion.voltage_saturated = execution_ours && execution.voltage_saturated;
    s_velocity_pi.saturated = !execution_ours || execution.current_limited
                           || execution.voltage_saturated || execution.request_expired;
    float torque_limit = reducer * kt * s_motion_iq_max_a;
    float torque;
    if (s_speed_mode) {
        torque = algo_motion_speed(
            &s_velocity_pi, s_target, s_motion.speed_rad_s, s_velocity_kp, s_velocity_ki, dt,
            torque_limit);
        /* 摩擦前馈按目标速度求值（平滑符号防零点抖动）；辨识期挂起；再按电流硬限制钳位 */
        if (!s_friction_suspended)
            torque += algo_friction_torque(&s_friction_model, s_target);
        torque = torque > torque_limit  ? torque_limit
               : torque < -torque_limit ? -torque_limit
                                        : torque;
    } else {
        torque = s_target;
    }
    s_motion.torque_nm = torque;
    s_motion.iq_a = torque / (reducer * kt);
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
