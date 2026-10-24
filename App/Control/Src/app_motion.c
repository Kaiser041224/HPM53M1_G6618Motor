/** @file app_motion.c
 * @brief motion实时状态唯一写者，静态游标相位标定
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_motion.h"
#include "algo_friction.h"
#include "algo_motion.h"
#include "algo_ramp.h"
#include "app_encoder.h"
#include "app_foc_current.h"
#include "app_foc.h"
#include "app_motor_params.h"
#include "app_param.h"
#include "app_ramfunc.h"
#include "app_software_params.h"
#include "foc_math.h"
#include "intf_clock.h"
#include "intf_sys.h"

#define APP_MOTION_SPEED_RATE_HZ    (15000.0f) /**< 15kHz 速度拍 */
#define APP_MOTION_POSITION_RATE_HZ (15000.0f) /**< 15kHz 位置拍 */

static app_motion_snapshot_t s_motion;
volatile float g_motion_shaft_angle_rad __attribute__((section(".noncacheable.bss")));
volatile float g_motion_shaft_angle_deg __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_motion_shaft_angle_valid __attribute__((section(".noncacheable.bss")));
/* 调试观测（.noncacheable，Ozone/RTT 直读；不参与控制）：
 * 速度纹波按 15kHz 速度拍统计 1ms 窗口（15 拍）峰峰值。 */
volatile float g_motion_speed_dbg_rad_s __attribute__((section(".noncacheable.bss")));
volatile float g_motion_speed_ripple_pp_dbg __attribute__((section(".noncacheable.bss")));
volatile float g_motion_speed_ripple_max_dbg __attribute__((section(".noncacheable.bss")));
#define APP_MOTION_DBG_WINDOW_SAMPLES (15U) /**< 1ms @15kHz */
static float s_speed_dbg_min, s_speed_dbg_max;
static uint32_t s_speed_dbg_samples;

/** @brief 速度调试窗口更新（仅单写者 app_motion_step 调用；热路径放 ILM） */
APP_ATTR_RAMFUNC
static void motion_debug_speed_update(float speed) {
    g_motion_speed_dbg_rad_s = speed;
    if (s_speed_dbg_samples == 0U) {
        s_speed_dbg_min = speed;
        s_speed_dbg_max = speed;
    } else {
        if (speed < s_speed_dbg_min) {
            s_speed_dbg_min = speed;
        }
        if (speed > s_speed_dbg_max) {
            s_speed_dbg_max = speed;
        }
    }
    s_speed_dbg_samples++;
    if (s_speed_dbg_samples >= APP_MOTION_DBG_WINDOW_SAMPLES) {
        float ripple = s_speed_dbg_max - s_speed_dbg_min;

        if (!foc_finite(ripple) || (ripple < 0.0f)) {
            ripple = 0.0f;
        }
        g_motion_speed_ripple_pp_dbg = ripple;
        if (ripple > g_motion_speed_ripple_max_dbg) {
            g_motion_speed_ripple_max_dbg = ripple;
        }
        s_speed_dbg_samples = 0U;
    }
}
static algo_motion_pi_t s_velocity_pi;
static float s_target, s_velocity_kp, s_velocity_ki;
static algo_friction_params_t s_friction_model; /**< 摩擦前馈模型（Tc/Kv/平滑；init/gains 消费） */
static bool s_friction_suspended;               /**< 辨识期挂起前馈（速度模式FF置零） */
static float s_velocity_lpf_coeff;              /**< 测速一阶滤波系数（参数 init 期消费） */
static float s_motion_iq_max_a;          /**< motion 命令电流硬限制[A]（力矩命令与PI输出上限） */
static float s_motion_speed_limit_rad_s; /**< 轴端实际速度上限[rad/s]（0=不限） */
static float s_motion_speed_limit_gain;  /**< 连续速度限幅增益[Nm/(rad/s)]（0=关闭） */
static float s_velocity_ramp_rate;       /**< 速度目标斜坡[rad/s²]（0=直通） */
static float s_torque_ramp_rate;         /**< 力矩目标斜坡[Nm/s]（0=直通） */
static float s_target_effective;         /**< 经过 ramp 的生效目标（速度/力矩模式） */
static bool s_target_ramp_primed;        /**< 生效目标已从当前状态起算 */
static algo_ramp_t s_position_traj;      /**< 位置梯形轨迹 */
static bool s_position_traj_ready;       /**< 轨迹对象已初始化 */
static volatile bool s_traj_reset;       /**< 新位置命令需要重规划轨迹（跨任务/ISR） */
static float s_traj_prev_pos;            /**< 上一拍轨迹位置，用于差分测速 */
static float s_output_direction = 1.0f;  /**< 用户出轴正方向（+1/-1；仅 motion 边界） */
static float s_previous_angle, s_zero_rad, s_previous_rotor;
static float s_previous_motor_rad; /**< 上一拍转子机械角（测速基准；输出侧速度=转子速度/减速比） */
static float s_calibration_sum, s_calibration_first;
static float s_calibration_phase;
static float s_calibration_reducer, s_calibration_output_ratio;
static uint32_t s_previous_cycle, s_previous_sequence, s_epoch, s_request_sequence;
static uint32_t s_previous_sample_cycle;                     /**< 上一个用于测速的配对快照时间 */
static float s_seconds_per_cycle;
static bool s_primed;
static float s_input_pos, s_input_vel_ff, s_input_torque_ff; /**< 位置模式输入（ODrive input_*） */
static float s_pos_gain, s_pos_filter_bw;     /**< 位置环增益（init/gains 消费；0=禁用/直通） */
static algo_motion_pos_filter_t s_pos_filter; /**< 输入二阶滤波（15kHz CH0 拍） */
static float s_vel_cmd_rad_s;                 /**< 位置环输出（同拍 position→velocity 交接） */
static float s_active_direction, s_active_kt, s_active_reducer;
static float s_calibrated_direction;
static uint32_t s_hold_samples;               /**< 当前连续坏拍数（active质量类保持窗口） */
static bool s_velocity_gap;                   /**< 保持恢复后首拍跳过跨隙测速差分 */
static float s_nearest_turn;                  /**< 最近解析的游标整数圈号（兜底测速基准） */
static bool s_have_nearest;
static uint32_t s_vernier_fallback_samples;   /**< 连续兜底样本数（超限判真失效） */
#define APP_MOTION_VERNIER_FALLBACK_MAX_SAMPLES (7500U) /**< 兜底上限500ms@15kHz */
static const app_motor_params_t* s_motion_motor_params; /**< init解析，仅读RAM参数对象 */

/** @brief 游标多圈解算参数是否可用（reducer 圈数 + 出轴/转子比） */
APP_ATTR_RAMFUNC
static bool motion_vernier_params_valid(void) {
    if (s_motion_motor_params == NULL) {
        return false;
    }
    float reducer = s_motion_motor_params->reducer_ratio;
    float output_ratio = s_motion_motor_params->encoder.output_ratio;

    if (!foc_finite(reducer) || (reducer < 1.0f) || !foc_finite(output_ratio)
        || (output_ratio <= 0.0f) || (output_ratio >= 1.0f)) {
        return false;
    }
    float expected_reducer = 1.0f / (1.0f - output_ratio);
    float reducer_delta = reducer - expected_reducer;

    return foc_finite(expected_reducer) && (reducer_delta <= 1.0e-3f)
        && (reducer_delta >= -1.0e-3f);
}

/* 保持窗口15kHz×300=20ms零力矩：游标残差坏拍簇（齿隙/扭振瞬态）可穿过；
 * 游标每拍独立重解绝对圈号，恢复无累计误差。>窗口判真失败。 */
#define APP_MOTION_HOLD_MAX_SAMPLES (300U)
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
    s_motion.speed_limited = false;
    s_motion.reason = reason;
    s_target_effective = 0;
    s_target_ramp_primed = false;
    s_traj_reset = false;
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
    g_motion_speed_dbg_rad_s = 0;
    g_motion_speed_ripple_pp_dbg = 0;
    g_motion_speed_ripple_max_dbg = 0;
    s_speed_dbg_min = 0;
    s_speed_dbg_max = 0;
    s_speed_dbg_samples = 0;
    s_primed = false;
    s_previous_sequence = 0;
    s_previous_cycle = 0;
    s_previous_sample_cycle = 0;
    s_previous_motor_rad = 0;
    s_hold_samples = 0;
    s_velocity_gap = false;
    s_have_nearest = false;
    s_nearest_turn = 0;
    s_vernier_fallback_samples = 0;
    s_zero_rad = 0;
    s_input_pos = 0;
    s_input_vel_ff = 0;
    s_input_torque_ff = 0;
    s_pos_filter = (algo_motion_pos_filter_t){0};
    s_vel_cmd_rad_s = 0;
    s_calibrated_direction = 0;
    s_motion_motor_params = app_motor_params_current();
    s_seconds_per_cycle = 1.0f / intf_clock_get_cpu_freq();
    s_velocity_pi = (algo_motion_pi_t){0};
    /* 力矩/速度环参数：YAML 出厂值 + flash 记忆覆盖（init 期消费；motion gains 在线改） */
    const app_software_params_t* software = app_software_params_current();
    s_output_direction = software->motion.output_direction;
    if (!foc_finite(s_output_direction)
        || ((s_output_direction != 1.0f) && (s_output_direction != -1.0f))) {
        s_output_direction = 1.0f;
    }
    s_motion.output_direction = s_output_direction;
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
    s_motion_speed_limit_gain = software->motion_speed_limit_gain;
    if (!foc_finite(s_motion_speed_limit_gain) || s_motion_speed_limit_gain < 0)
        s_motion_speed_limit_gain = 0;
    s_velocity_ramp_rate = software->motion_input.vel_ramp_rate;
    if (!foc_finite(s_velocity_ramp_rate) || s_velocity_ramp_rate < 0)
        s_velocity_ramp_rate = 0;
    s_torque_ramp_rate = software->motion_input.torque_ramp_rate;
    if (!foc_finite(s_torque_ramp_rate) || s_torque_ramp_rate < 0)
        s_torque_ramp_rate = 0;
    s_target_effective = 0;
    s_target_ramp_primed = false;
    s_traj_reset = false;
    s_traj_prev_pos = 0;
    s_position_traj_ready = false;
    {
        float traj_vel = software->position_traj.trajectory_vel_limit;
        float traj_accel = software->position_traj.trajectory_accel_limit;

        if (foc_finite(traj_vel) && (traj_vel > 0.0f) && foc_finite(traj_accel)
            && (traj_accel > 0.0f)) {
            algo_ramp_cfg_t traj_cfg = {
                .mode = ALGO_RAMP_MODE_TRAPEZOIDAL,
                .rate = traj_vel,
                .step_time_s = 1.0f / APP_MOTION_POSITION_RATE_HZ,
                .target = 0.0f,
                .initial = 0.0f,
                .accel_max = traj_accel,
                .duration_s = 0.0f,
            };

            algo_ramp_ctor(&s_position_traj);
            s_position_traj_ready = s_position_traj.init(&s_position_traj, &traj_cfg) == 0;
        }
    }
    s_pos_gain = software->control.position_loop.pos_gain;
    if (!foc_finite(s_pos_gain) || s_pos_gain < 0 || s_pos_gain > 100)
        s_pos_gain = 0;
    s_pos_filter_bw = software->control.position_loop.input_filter_bandwidth;
    if (!foc_finite(s_pos_filter_bw) || s_pos_filter_bw < 0 || s_pos_filter_bw > 100)
        s_pos_filter_bw = 0;
    motion_record_t record;
    float phase_limit =
        motion_vernier_params_valid() ? (1.0f / s_motion_motor_params->reducer_ratio) : 0.0f;
    if (motion_vernier_params_valid()
        && app_param_load(APP_PARAM_KEY_MOTION, &record, sizeof(record)) == 0
        && record.version == 1 && foc_finite(record.phase) && record.phase >= 0
        && record.phase < phase_limit
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
    if (!motion_vernier_params_valid()) {
        intf_sys_irq_restore(st);
        return -1;
    }
    s_calibration_reducer = s_motion_motor_params->reducer_ratio;
    s_calibration_output_ratio = s_motion_motor_params->encoder.output_ratio;
    s_motion.calibrating = true;
    s_motion.feedback_valid = false;
    g_motion_shaft_angle_valid = 0;
    s_motion.calibration_samples = 0;
    s_motion.calibration_done = false;
    s_motion.reason = APP_MOTION_REASON_NONE;
    s_calibration_sum = 0;
    s_primed = false;
    s_have_nearest = false;                       /* 相位重标定：旧圈号基准失效 */
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
    if (!motion_vernier_params_valid()
        || (s_calibration_reducer != s_motion_motor_params->reducer_ratio)
        || (s_calibration_output_ratio != s_motion_motor_params->encoder.output_ratio)) {
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
    s_have_nearest = false;                       /* 新相位：旧圈号基准失效 */
    s_pos_filter = (algo_motion_pos_filter_t){0}; /* 位置坐标系随相位重建 */
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
    s_pos_filter = (algo_motion_pos_filter_t){0};
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
    if (app_foc_get_state() != APP_FOC_STATE_OFF || app_foc_stop_pending() || !s_motion.calibrated
        || s_motion.calibrating
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
static int motion_command(float target, float vel_ff, float torque_ff, app_motion_mode_t mode) {
    if (!foc_finite(target) || !foc_finite(vel_ff) || !foc_finite(torque_ff))
        return APP_MOTION_CMD_BAD_PARAM;
    uint32_t st = intf_sys_irq_save();
    bool was_active = s_motion.active;
    app_motion_mode_t old_mode = s_motion.mode;
    float kt = s_motion_motor_params->kt_nm_per_a;
    float reducer = s_motion_motor_params->reducer_ratio;
    if (!foc_finite(kt) || kt <= 0 || !foc_finite(reducer) || reducer <= 0) {
        intf_sys_irq_restore(st);
        return APP_MOTION_CMD_BAD_PARAM;
    }
    if ((mode == APP_MOTION_MODE_POSITION) && !motion_vernier_params_valid()) {
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
    /* 力矩/torque_ff 受 motion_iq_max_a 硬限制；速度/vel_ff 受 motion_speed_limit（0=不限）；
     * 位置目标=单圈绝对值 [0,2π]（终端0~360°），见 over_angle */
    float torque_cap = reducer * kt * s_motion_iq_max_a;
    bool over_torque = (mode == APP_MOTION_MODE_TORQUE && fabsf(target) > torque_cap)
                    || (mode == APP_MOTION_MODE_POSITION && fabsf(torque_ff) > torque_cap);
    bool over_speed =
        s_motion_speed_limit_rad_s > 0
        && ((mode == APP_MOTION_MODE_SPEED && fabsf(target) > s_motion_speed_limit_rad_s)
            || (mode == APP_MOTION_MODE_POSITION && fabsf(vel_ff) > s_motion_speed_limit_rad_s));
    /* 位置目标=单圈绝对值 [0, 2π]（终端0~360°）；360°=2π 归 0 */
    bool over_angle = mode == APP_MOTION_MODE_POSITION && (target < 0 || target > FOC_TWO_PI_F);
    if (over_torque || over_speed || over_angle) {
        intf_sys_irq_restore(st);
        return APP_MOTION_CMD_TARGET_LIMIT;
    }
    /* 用户坐标 → 内部物理坐标：输出方向只在这里翻转，FOC/编码器方向保持硬件事实。
     * 位置/速度/转矩一致翻转，保证“正命令 → 出轴正方向运动”。 */
    target *= s_output_direction;
    vel_ff *= s_output_direction;
    torque_ff *= s_output_direction;
    /* 位置级联内环=速度PI：位置模式同时要求 kp 与 pos_gain 已配置 */
    if ((mode == APP_MOTION_MODE_SPEED && s_velocity_kp <= 0)
        || (mode == APP_MOTION_MODE_POSITION && (s_pos_gain <= 0 || s_velocity_kp <= 0))) {
        intf_sys_irq_restore(st);
        return APP_MOTION_CMD_GAIN_NOT_SET;
    }
    if (app_foc_stop_pending()
        || (app_foc_get_state() != APP_FOC_STATE_READY
            && app_foc_get_state() != APP_FOC_STATE_RUN)) {
        intf_sys_irq_restore(st);
        return APP_MOTION_CMD_FOC_NOT_READY;
    }
    if (s_motion.active && s_motion.mode != mode) {
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
    s_motion.mode = mode;
    if (mode == APP_MOTION_MODE_POSITION) {
        s_input_pos = foc_wrap_2pi(target);
        s_input_vel_ff = vel_ff;
        s_input_torque_ff = torque_ff;
        s_traj_reset = true; /* 新位置命令：从当前反馈重规划梯形轨迹 */
    } else {
        s_target = target;
    }
    if (!was_active || (old_mode != mode)) {
        /* 首次进入/切换模式：ramp 从当前可观测状态起算，避免从 0 突跳。 */
        s_target_ramp_primed = false;
        s_target_effective = (mode == APP_MOTION_MODE_SPEED) ? s_motion.speed_rad_s : 0.0f;
    }
    s_motion.active = true;
    s_motion.speed_limited = false;
    s_motion.reason = APP_MOTION_REASON_NONE;
    s_active_direction = s_motion_motor_params->encoder.direction;
    s_active_kt = kt;
    s_active_reducer = reducer;
    intf_sys_irq_restore(st);
    return 0;
}
int app_motion_torque(float torque) { return motion_command(torque, 0, 0, APP_MOTION_MODE_TORQUE); }
int app_motion_speed(float speed) { return motion_command(speed, 0, 0, APP_MOTION_MODE_SPEED); }
int app_motion_position(float position_rad, float vel_ff_rad_s, float torque_ff_nm) {
    return motion_command(position_rad, vel_ff_rad_s, torque_ff_nm, APP_MOTION_MODE_POSITION);
}
int app_motion_position_gains(float pos_gain, float filter_bandwidth_rad_s) {
    if (!foc_finite(pos_gain) || !foc_finite(filter_bandwidth_rad_s) || pos_gain < 0
        || pos_gain > 100 || filter_bandwidth_rad_s < 0 || filter_bandwidth_rad_s > 100)
        return APP_MOTION_CMD_BAD_PARAM;
    uint32_t st = intf_sys_irq_save();
    if (s_motion.active) { /* 安全意图：运动驱动中不改环路增益 */
        intf_sys_irq_restore(st);
        return APP_MOTION_CMD_FOC_NOT_READY;
    }
    s_pos_gain = pos_gain;
    s_pos_filter_bw = filter_bandwidth_rad_s;
    app_software_params_t* software = app_software_params_mutable();
    software->control.position_loop.pos_gain = pos_gain;
    software->control.position_loop.input_filter_bandwidth = filter_bandwidth_rad_s;
    intf_sys_irq_restore(st);
    return 0;
}
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
void app_motion_tick(void) {
    /* 单一 15kHz 实时入口：位置模式先产生速度设定，再由速度环消费。
     * 位置使用上一拍反馈快照（1/15kHz 延迟）；随后 app_motion_step 刷新反馈并算速度。
     * 速度/力矩模式完全跳过位置分支，避免无谓的位置滤波/轨迹/游标解算。
     * 后续多圈/MIT 等模式在此按 mode 分发；当前只有 POSITION 需要前置位置解算。 */
    if (s_motion.active && (s_motion.mode == APP_MOTION_MODE_POSITION)) {
        app_motion_position_step();
    }
    app_motion_step();
}

APP_ATTR_RAMFUNC
void app_motion_step(void) {
    app_encoder_rotor_snapshot_t rotor;
    uint16_t raw;
    uint32_t now = intf_clock_get_cycle();
    bool need_pair = s_motion.calibrating
                     || (s_motion.active && (s_motion.mode == APP_MOTION_MODE_POSITION));
    /* 速度/力矩模式只用转子/observer：不读游标 pair，也不做 vernier 解算。
     * 位置模式与标定仍要求完整 rotor+vernier。 */
    bool rotor_only = s_motion.active && !s_motion.calibrating
                      && (s_motion.mode != APP_MOTION_MODE_POSITION);
    bool pair_ok = false;
    float reducer = s_motion_motor_params->reducer_ratio;
    float output_ratio = s_motion_motor_params->encoder.output_ratio;

    if (rotor_only) {
        if ((app_encoder_get_rotor_snapshot(&rotor) != 0) || !rotor.valid) {
            s_motion.sample_failure_count++;
            motion_reject(APP_MOTION_REASON_SAMPLE);
            return;
        }
        raw = 0U;
    } else {
        pair_ok = (app_encoder_output_sample(&rotor, &raw) == 0);
        if (!pair_ok) {
            /* 位置/标定必须完整 pair；非活动的诊断路径允许转子快照 */
            if (need_pair || (app_encoder_get_rotor_snapshot(&rotor) != 0) || !rotor.valid) {
                s_motion.sample_failure_count++;
                motion_reject(APP_MOTION_REASON_SAMPLE);
                return;
            }
            raw = 0U;
        }
    }

    /* P0-B：速度环与 FOC 共用 30kHz observer 的轴端速度；无效时退回转子差分。
     * 直读 ISR 发布的 .noncacheable 快照，避免 15kHz 热路径调用 Flash 访问器。 */
    bool observer_speed_ok =
        g_foc_speed_out_valid && foc_finite(g_foc_speed_out_rad_s);
    if (rotor.seq == s_previous_sequence) {
        if (!rotor_only) {
            s_motion.duplicate_count++;
            motion_reject(APP_MOTION_REASON_DUPLICATE);
            return;
        }
        /* 速度/力矩：无新转子样本时继续用 observer 或上拍速度；不因游标/配对节奏中断 */
    }
    s_previous_sequence = rotor.seq;
    if (!foc_finite(rotor.rad) || rotor.rad < 0 || rotor.rad >= FOC_TWO_PI_F) {
        motion_reject(APP_MOTION_REASON_SAMPLE);
        return;
    }
    if (!foc_finite(reducer) || reducer <= 0.0f
        || (!rotor_only && !motion_vernier_params_valid())) {
        motion_reject(APP_MOTION_REASON_PARAMETER);
        return;
    }
    float u = rotor.rad / FOC_TWO_PI_F, v = (float)raw / 65536.0f;
    if (s_motion.calibrating) {
        if (app_foc_get_state() != APP_FOC_STATE_OFF) {
            s_motion.calibrating = false;
            s_motion.reason = APP_MOTION_REASON_FOC_STATE;
            return;
        }
        float scaled_phase = (v - output_ratio * u) * reducer;
        int32_t phase_whole = (int32_t)scaled_phase;
        if ((float)phase_whole > scaled_phase)
            phase_whole--;
        float phase_limit = 1.0f / reducer;
        float phase = (scaled_phase - (float)phase_whole) * phase_limit;
        if (s_motion.calibration_samples == 0) {
            s_calibration_first = phase;
            s_previous_rotor = u;
        }
        float difference = motion_delta((phase - s_calibration_first) * reducer) / reducer;
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
                mean += phase_limit;
            else if (mean >= phase_limit)
                mean -= phase_limit;
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
    if (!pair_ok) {
        angle = s_previous_angle; /* 速度/力矩：保持最后游标角，仅诊断；速度由 observer 提供 */
    } else if (algo_motion_vernier(u, v, s_motion.phase_turns, reducer, output_ratio, &angle)
               == 0) {
        s_vernier_fallback_samples = 0;
        s_nearest_turn = angle * (reducer / FOC_TWO_PI_F) - u;
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
        angle = (s_nearest_turn + u) * (FOC_TWO_PI_F / reducer);
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
    /* 测速：observer 有效时直接用其轴端速度；否则退回转子编码器差分+LPF。
     * 不经过游标多圈解模糊，避免齿隙/扭振/解码噪声进入速度环；输出角仍只用于位置。 */
    float motor_delta = foc_wrap_pm_pi(rotor.rad - s_previous_motor_rad);
    if (observer_speed_ok) {
        /* observer 已含 PLL 滤波：不再叠加速度 LPF，避免双滤波滞后 */
        s_motion.speed_rad_s = g_foc_speed_out_rad_s;
        s_velocity_gap = false;
    } else if (s_velocity_gap) {
        s_velocity_gap = false; /* 跨保持间隙的差分不进入测速滤波 */
    } else if (s_primed && sample_dt > 0 && sample_dt < .001f) {
        float velocity = (motor_delta * direction) / sample_dt / reducer;
        s_motion.speed_rad_s += s_velocity_lpf_coeff * (velocity - s_motion.speed_rad_s);
    } else if (!s_primed) {
        s_motion.speed_rad_s = 0;
    }
    motion_debug_speed_update(s_motion.speed_rad_s);
    s_previous_motor_rad = rotor.rad;
    s_previous_angle = angle;
    s_previous_cycle = now;
    s_previous_sample_cycle = rotor.timestamp_cycles;
    s_motion.shaft_rad = foc_wrap_2pi(angle - (s_motion.zero_valid ? s_zero_rad : 0));
    s_primed = true;
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
    if (!foc_finite(kt) || kt <= 0 || kt != s_active_kt || reducer != s_active_reducer
        || direction != s_active_direction) {
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
                           || execution.voltage_saturated || execution.request_expired
                           || s_motion.speed_limited;
    float motion_iq_limit = s_motion_iq_max_a;
    float dynamic_limit = app_foc_current_get_i_limit();
    if ((dynamic_limit > 0.0f) && (dynamic_limit < motion_iq_limit)) {
        motion_iq_limit = dynamic_limit;
    }
    float torque_limit = reducer * kt * motion_iq_limit;
    float ramp_dt = (dt > 0.0f && dt <= 0.001f) ? dt : (1.0f / APP_MOTION_SPEED_RATE_HZ);
    float torque;
    bool speed_limited = false;

    if (!s_target_ramp_primed) {
        /* 首次进入或切换模式：速度从当前反馈起算，力矩从 0 起算。 */
        s_target_effective =
            (s_motion.mode == APP_MOTION_MODE_SPEED) ? s_motion.speed_rad_s : 0.0f;
        s_target_ramp_primed = true;
    }

    if (s_motion.mode != APP_MOTION_MODE_TORQUE) {
        /* 位置模式速度目标=位置环输出（15kHz CH0 拍）；速度模式=ramp 后的命令目标 */
        float target;
        if (s_motion.mode == APP_MOTION_MODE_POSITION) {
            target = s_vel_cmd_rad_s;
        } else {
            s_target_effective = algo_motion_ramp(
                s_target_effective, s_target, s_velocity_ramp_rate, ramp_dt);
            target = s_target_effective;
        }
        torque = algo_motion_speed(
            &s_velocity_pi, target, s_motion.speed_rad_s, s_velocity_kp, s_velocity_ki, dt,
            torque_limit);
        /* 摩擦前馈按目标速度求值（平滑符号防零点抖动）；辨识期挂起；再按电流硬限制钳位 */
        if (!s_friction_suspended)
            torque += algo_friction_torque(&s_friction_model, target);
        if (s_motion.mode == APP_MOTION_MODE_POSITION)
            torque += s_input_torque_ff;
    } else {
        s_target_effective = algo_motion_ramp(
            s_target_effective, s_target, s_torque_ramp_rate, ramp_dt);
        torque = s_target_effective;
    }

    torque = torque > torque_limit  ? torque_limit
           : torque < -torque_limit ? -torque_limit
                                    : torque;
    torque = algo_motion_limit_vel(
        torque, s_motion.speed_rad_s, s_motion_speed_limit_rad_s, s_motion_speed_limit_gain,
        &speed_limited);
    if (speed_limited) {
        /* 限速正在干预：冻结速度积分，避免退出限速后积分突释。 */
        s_velocity_pi.saturated = true;
    }
    s_motion.speed_limited = speed_limited;
    s_motion.torque_nm = torque;
    s_motion.iq_a = torque / (reducer * kt);
    app_foc_current_request_t request = {0, s_motion.iq_a, ++s_request_sequence, s_epoch, true};
    if (app_foc_submit_current(&request) != 0)
        motion_clear_output(APP_MOTION_REASON_SUBMIT);
}
APP_ATTR_RAMFUNC
void app_motion_position_step(void) {
    /* ODrive 级联：滤波输入（input_pos/input_vel）→ 位置P → 速度目标。
     * 反馈（shaft_rad 单圈绝对角）取自上一拍 app_motion_step 的反馈快照；
     * 单 tick 内 position→velocity 串行，零 SPI。 */
    if (!s_motion.active || s_motion.mode != APP_MOTION_MODE_POSITION || !s_motion.feedback_valid)
        return;

    float pos_setpoint;
    float vel_setpoint;
    if (s_position_traj_ready) {
        /* 轨迹模式由梯形轨迹自己生成 vel；input_vel_ff 不再叠加，避免双计。 */
        if (s_traj_reset) {
            float delta = foc_wrap_pm_pi(s_input_pos - s_motion.shaft_rad);
            float target = s_motion.shaft_rad + delta;

            (void)s_position_traj.set_current(&s_position_traj, s_motion.shaft_rad);
            (void)s_position_traj.set_target(&s_position_traj, target);
            s_traj_prev_pos = s_motion.shaft_rad;
            s_traj_reset = false;
        }
        pos_setpoint = s_position_traj.step(&s_position_traj);
        vel_setpoint = (pos_setpoint - s_traj_prev_pos) * APP_MOTION_POSITION_RATE_HZ;
        s_traj_prev_pos = pos_setpoint;
    } else {
        algo_motion_pos_filter_step(
            &s_pos_filter, s_input_pos, s_input_vel_ff, s_pos_filter_bw, 1.0f / APP_MOTION_POSITION_RATE_HZ);
        pos_setpoint = s_pos_filter.pos_setpoint_rad;
        vel_setpoint = s_pos_filter.vel_setpoint_rad_s;
    }

    float vel_cmd = algo_motion_position(pos_setpoint, s_motion.shaft_rad, s_pos_gain, vel_setpoint);
    if (s_motion_speed_limit_rad_s > 0)
        vel_cmd = vel_cmd > s_motion_speed_limit_rad_s  ? s_motion_speed_limit_rad_s
                : vel_cmd < -s_motion_speed_limit_rad_s ? -s_motion_speed_limit_rad_s
                                                        : vel_cmd;
    s_vel_cmd_rad_s = vel_cmd;
    s_motion.pos_setpoint_rad = foc_wrap_2pi(pos_setpoint); /* 显示取模单圈 */
    s_motion.vel_cmd_rad_s = vel_cmd;
}
void app_motion_get_snapshot(app_motion_snapshot_t* out) {
    if (out) {
        uint32_t st = intf_sys_irq_save();
        *out = s_motion;
        intf_sys_irq_restore(st);
    }
}
