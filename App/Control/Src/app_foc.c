/**
 * @file    app_foc.c
 * @brief   FOC 编排实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_foc.h"

#include "app_3phase_inverter.h"
#include "app_adc.h"
#include "app_encoder.h"
#include "app_fault.h"
#include "app_hardware_params.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "foc_angle.h"
#include "foc_math.h"
#include "intf_clock.h"

#define APP_FOC_SPEED_LPF_HZ (100.0f) /**< ωe 估计低通截止 [Hz] */

static app_foc_state_t s_state;
static foc_angle_t s_angle;
static app_foc_angle_source_t s_angle_src;
static float s_forced_theta;
static float s_i_d_ref;
static float s_i_q_ref;
static uint32_t s_rotor_seq_last; /**< 上一拍采样序号 */
static bool s_rotor_seq_valid;    /**< 序号已建立 */
static bool s_angle_ready;        /**< 角度链初始化成功 */
static bool s_initialized;        /**< app_foc_init 已执行 */

/* Ozone 观测：FOC 单拍耗时 [cycle]（.noncacheable.bss，调试器直读） */
volatile uint32_t g_foc_loop_cycles __attribute__((section(".noncacheable.bss")));

/**
 * @brief 电角度链初始化（参数来源 motor 域）
 * @return 0 = 成功；-1 = 参数非法（角度链不可用，enable 将拒绝）
 */
static int app_foc_angle_init(void) {
    const app_motor_params_t* motor = app_motor_params_current();
    foc_angle_cfg_t cfg;

    cfg.pole_pairs = motor->pole_pairs;
    cfg.direction = motor->encoder.direction;
    cfg.offset_rad = motor->encoder.electrical_offset_rad;
    cfg.speed_lpf_hz = APP_FOC_SPEED_LPF_HZ;
    cfg.sample_time_s = 1.0f / (float)app_hardware_params_current()->inverter.pwm_freq_hz;

    foc_angle_ctor(&s_angle);
    return s_angle.init(&s_angle, &cfg);
}

void app_foc_init(void) {
    s_state = APP_FOC_STATE_OFF;
    s_angle_src = APP_FOC_ANGLE_ENCODER;
    s_forced_theta = 0.0f;
    s_i_d_ref = 0.0f;
    s_i_q_ref = 0.0f;
    s_angle_ready = (app_foc_angle_init() == 0);
    app_foc_current_init();
    s_initialized = true;
}

/**
 * @brief 读取转子机械角（未加软件零点）[rad]；含采样停摆检测
 * @return true = 有效（本拍采样序号已推进）
 * @note 序号未推进（采样停摆）→ 视为无效，调用方应输出零矢量
 */
static bool app_foc_read_rotor_rad(float* theta_m_rad) {
    uint16_t raw;
    bool valid;
    uint32_t seq;

    if (app_encoder_get_rotor_raw(&raw, &valid, &seq) != 0) {
        return false;
    }
    if (!valid) {
        return false;
    }
    if (s_rotor_seq_valid && (seq == s_rotor_seq_last)) {
        return false; /* 采样未推进：拒绝使用陈旧角 */
    }
    s_rotor_seq_last = seq;
    s_rotor_seq_valid = true;

    *theta_m_rad = (float)raw * (FOC_TWO_PI_F / 65536.0f);
    return true;
}

/**
 * @brief 25kHz 单拍主体（由 app_foc_run_once 计时包裹）
 */
static void app_foc_run_body(void) {
    float theta_e = 0.0f;
    float omega_e = 0.0f;
    float theta_m;

    if (!s_initialized) {
        return;
    }

    /* 故障门控：活动状态下 fault != NORMAL → FAULT（仅迁移时动作一次） */
    if (s_state == APP_FOC_STATE_FAULT) {
        return;
    }
    if ((s_state != APP_FOC_STATE_OFF)
        && (app_fault_get_state() != APP_FAULT_STATE_NORMAL)) {
        app_foc_current_zero_vector();
        app_3phase_inverter_disable();
        s_state = APP_FOC_STATE_FAULT;
        return;
    }

    if ((s_state != APP_FOC_STATE_READY) && (s_state != APP_FOC_STATE_RUN)
        && (s_state != APP_FOC_STATE_CALIB)) {
        return;
    }

    /* 角度 */
    if (s_angle_src == APP_FOC_ANGLE_FORCED) {
        theta_e = foc_wrap_2pi(s_forced_theta);
        omega_e = 0.0f;
    } else {
        if (!app_foc_read_rotor_rad(&theta_m)) {
            app_foc_current_zero_vector();
            return;
        }
        theta_e = s_angle.step(&s_angle, theta_m, &omega_e);
    }

    /* 电流环 */
    (void)app_foc_current_run(theta_e, omega_e, s_i_d_ref, s_i_q_ref, NULL, NULL);

    if ((s_state == APP_FOC_STATE_READY) && ((s_i_d_ref != 0.0f) || (s_i_q_ref != 0.0f))) {
        s_state = APP_FOC_STATE_RUN;
    }
}

void app_foc_run_once(void) {
    uint32_t t0 = intf_clock_get_cycle();

    app_foc_run_body();
    g_foc_loop_cycles = intf_clock_get_cycle() - t0;
}

int app_foc_enable(void) {
    const app_motor_params_t* motor = app_motor_params_current();
    const app_software_params_t* software = app_software_params_current();
    uint16_t raw;
    bool valid;

    if (!s_initialized) {
        return -1;
    }
    if (!s_angle_ready || !app_foc_current_is_ready()) {
        return -1; /* 角度链/电流环初始化失败：禁止使能 */
    }
    if (s_state == APP_FOC_STATE_FAULT) {
        return -1; /* 需先 app_foc_disable() 回 OFF */
    }
    if (s_state != APP_FOC_STATE_OFF) {
        return 0; /* 已使能：幂等 */
    }
    if (app_fault_get_state() != APP_FAULT_STATE_NORMAL) {
        return -1;
    }
    if (!app_adc_is_valid()) {
        return -1;
    }
    if ((app_encoder_get_rotor_raw(&raw, &valid, NULL) != 0) || !valid) {
        return -1;
    }
    if ((motor->pole_pairs == 0U) || (software->control.limits.duty_max <= 0.5f)
        || (software->control.limits.duty_max > 1.0f)) {
        return -1;
    }
    /* direction 元数据范围 [-1,1] 无法表达"仅 ±1"：运行期显式校验 */
    if ((motor->encoder.direction != 1.0f) && (motor->encoder.direction != -1.0f)) {
        return -1;
    }
    if (!foc_finite(motor->encoder.electrical_offset_rad)) {
        return -1;
    }

    /* live 参数热更新 */
    s_angle.set_offset(&s_angle, motor->encoder.electrical_offset_rad,
                       motor->encoder.direction);

    /* 注：app_3phase_inverter_enable() 内含 +12V 栅极供电稳定等待（约 10ms 阻塞）。
     * 该窗口内桥处于关闭态（PWM 未启动），无电流风险；但 25kHz 环与 L2/L3 保护暂停。
     * 与既有 V/F 启动路径同构；非阻塞桥使能为 v2 项（spec §10）。 */
    if (app_3phase_inverter_enable() != 0) {
        return -1;
    }
    app_foc_current_reset();
    s_angle.reset(&s_angle);
    s_rotor_seq_valid = false; /* 重新建立采样序号基准 */
    s_angle_src = APP_FOC_ANGLE_ENCODER;
    s_i_d_ref = 0.0f;
    s_i_q_ref = 0.0f;
    app_foc_current_zero_vector();
    s_state = APP_FOC_STATE_READY;
    return 0;
}

void app_foc_disable(void) {
    app_foc_current_zero_vector();
    app_3phase_inverter_disable();
    s_i_d_ref = 0.0f;
    s_i_q_ref = 0.0f;
    s_angle_src = APP_FOC_ANGLE_ENCODER;
    s_state = APP_FOC_STATE_OFF;
}

int app_foc_set_iq_ref(float i_q_a) {
    float limit;

    if ((s_state != APP_FOC_STATE_READY) && (s_state != APP_FOC_STATE_RUN)) {
        return -1;
    }
    if (!foc_finite(i_q_a)) {
        return -1;
    }
    limit = app_software_params_current()->control.limits.i_q_max_a;
    if (!foc_finite(limit) || (limit <= 0.0f)) {
        return -1; /* 限幅值非法：拒绝（防 NaN 穿透/负值反号） */
    }
    if (i_q_a > limit) {
        i_q_a = limit;
    } else if (i_q_a < -limit) {
        i_q_a = -limit;
    }
    s_i_q_ref = i_q_a;
    return 0;
}

int app_foc_set_id_ref(float i_d_a) {
    if ((s_state != APP_FOC_STATE_READY) && (s_state != APP_FOC_STATE_RUN)) {
        return -1;
    }
    if (!foc_finite(i_d_a)) {
        return -1;
    }
    s_i_d_ref = i_d_a;
    return 0;
}

int app_foc_set_angle_source(app_foc_angle_source_t src, float theta_e_rad) {
    if ((src != APP_FOC_ANGLE_ENCODER) && (src != APP_FOC_ANGLE_FORCED)) {
        return -1;
    }
    if (!foc_finite(theta_e_rad)) {
        return -1;
    }
    if ((s_state != APP_FOC_STATE_READY) && (s_state != APP_FOC_STATE_RUN)
        && (s_state != APP_FOC_STATE_CALIB)) {
        return -1;
    }
    s_angle_src = src;
    s_forced_theta = theta_e_rad;
    return 0;
}

float app_foc_get_iq_ref(void) { return s_i_q_ref; }

app_foc_state_t app_foc_get_state(void) { return s_state; }

bool app_foc_is_active(void) { return (s_state != APP_FOC_STATE_OFF); }

void app_foc_get_snapshot(app_foc_current_snapshot_t* out) { app_foc_current_get_snapshot(out); }

int app_foc_enter_calib(void) {
    if ((s_state != APP_FOC_STATE_READY) && (s_state != APP_FOC_STATE_RUN)) {
        return -1;
    }
    app_foc_current_reset();
    s_angle_src = APP_FOC_ANGLE_FORCED;
    s_forced_theta = 0.0f;
    s_i_d_ref = 0.0f;
    s_i_q_ref = 0.0f;
    s_state = APP_FOC_STATE_CALIB;
    return 0;
}

void app_foc_exit_calib(void) {
    if (s_state == APP_FOC_STATE_CALIB) {
        s_angle_src = APP_FOC_ANGLE_ENCODER;
        s_i_d_ref = 0.0f;
        s_i_q_ref = 0.0f;
        s_angle.reset(&s_angle);
        s_state = APP_FOC_STATE_READY;
    }
}

void app_foc_calib_set_excitation(float theta_e_rad, float i_d_ref, float i_q_ref) {
    if (s_state == APP_FOC_STATE_CALIB) {
        s_forced_theta = theta_e_rad;
        s_i_d_ref = i_d_ref;
        s_i_q_ref = i_q_ref;
    }
}

void app_foc_apply_encoder_offset(float offset_rad, float direction) {
    s_angle.set_offset(&s_angle, offset_rad, direction);
}

bool app_foc_fault_gate(void) { return (s_state == APP_FOC_STATE_FAULT); }
