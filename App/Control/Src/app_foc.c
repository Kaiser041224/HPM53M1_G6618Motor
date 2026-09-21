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
#include "app_analog_signal.h"
#include "app_encoder.h"
#include "app_fault.h"
#include "app_hardware_params.h"
#include "app_motor_identify.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "foc_angle.h"
#include "foc_math.h"
#include "intf_clock.h"

#define APP_FOC_SPEED_LPF_HZ (100.0f) /**< ωe 估计低通截止 [Hz] */
/* 限速滞环：超限切断，回落至 85% 才恢复（ωe 噪声/回摆不再造成转矩断续） */
#define APP_FOC_SPEED_LIMIT_RESTORE (0.85f)

static app_foc_state_t s_state;
static foc_angle_t s_angle;
static app_foc_angle_source_t s_angle_src;
static float s_forced_theta;
static float s_i_d_ref;
static float s_i_q_ref_cmd; /**< 操作员 q 轴给定 [A]（未限速） */
static float s_i_q_ref;     /**< 生效 q 轴给定 [A]（含转矩模式限速） */
static uint32_t s_rotor_seq_last; /**< 上一拍采样序号 */
static bool s_rotor_seq_valid;    /**< 序号已建立 */
static bool s_angle_ready;        /**< 角度链初始化成功 */
static bool s_initialized;        /**< app_foc_init 已执行 */
static uint32_t s_last_cycle;     /**< 上一拍 FOC 调用时刻 [cycle]（实测 dt 用） */
static bool s_dt_valid;           /**< 已建立上一拍时刻 */
static float s_dt_s;              /**< 实测调用间隔 [s]（0 = 首拍/异常） */
static bool s_speed_limited;      /**< 限速滞环状态（避免阈值附近转矩断续） */

/* Ozone 观测：FOC 单拍耗时 [cycle]（.noncacheable.bss，调试器直读） */
volatile uint32_t g_foc_loop_cycles __attribute__((section(".noncacheable.bss")));
/* Ozone 观测：FOC 调用间隔 [µs]（25kHz 标称；抖动/丢拍时明显大于 40） */
volatile uint32_t g_foc_loop_dt_us __attribute__((section(".noncacheable.bss")));

/**
 * @brief 实测本拍与上一拍的调用间隔 [s]
 * @note 主循环节拍存在抖动/丢拍（USB/终端/打印同循环），固定 1/25kHz 换算 ωe
 *       会产生数倍尖峰；首拍返回 0（角度链保持 ωe 不更新）
 */
static float app_foc_measure_dt(void) {
    uint32_t now = intf_clock_get_cycle();
    float dt = 0.0f;

    if (s_dt_valid) {
        dt = (float)(now - s_last_cycle) / (float)intf_clock_get_cpu_freq();
    }
    s_last_cycle = now;
    s_dt_valid = true;
    return dt;
}

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
    s_i_q_ref_cmd = 0.0f;
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
    bool valid;
    uint32_t seq;

    /* 按器件分辨率的原始机械角（未加软件零点；不硬编码 16bit） */
    if (app_encoder_get_rotor_rad(theta_m_rad, &valid, &seq) != 0) {
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
    /* 快速过流跳闸（电流环内置）：零矢量已由保护路径输出，此处关桥并锁存 FAULT */
    if ((s_state != APP_FOC_STATE_OFF) && app_foc_current_is_tripped()) {
        app_3phase_inverter_disable();
        s_state = APP_FOC_STATE_FAULT;
        return;
    }

    if (s_state == APP_FOC_STATE_OFF) {
        /* OFF：仅刷新观测（角度 + 实测电流；不写桥、不跑电流环）。
         * 桥已关闭 → 实测电流应为 0（±噪声）；快照同步刷新，避免"OFF 仍有电流"的误读。 */
        app_analog_values_t values;

        if (s_angle_ready && app_foc_read_rotor_rad(&theta_m)) {
            float omega_off = 0.0f;
            float theta_off = s_angle.step(&s_angle, theta_m, s_dt_s, &omega_off);

            g_foc_current_snapshot.theta_e_rad = theta_off;
            g_foc_current_snapshot.omega_e_rad_s = omega_off;
            if (app_analog_signal_read_all(&values)) {
                float i_alpha, i_beta, s, c;

                foc_clarke(values.i_u_a, values.i_v_a, values.i_w_a, &i_alpha, &i_beta);
                foc_sincos(theta_off, &s, &c);
                foc_park_sc(i_alpha, i_beta, s, c, &g_foc_current_snapshot.i_d_a,
                            &g_foc_current_snapshot.i_q_a);
                g_foc_current_snapshot.v_bus_v = values.v_bus_v;
            }
        }
        return;
    }

    if ((s_state != APP_FOC_STATE_READY) && (s_state != APP_FOC_STATE_RUN)
        && (s_state != APP_FOC_STATE_CALIB)) {
        return;
    }

    /* 开环电压诊断（vtest）：优先于电流环；由 app_foc_current 自行调制输出 */
    if (app_foc_current_vtest_active()) {
        (void)app_foc_current_vtest_step();
        return;
    }

    /* 辨识模式：由辨识模块提供激励（强制角 + 电流给定） */
    if (s_state == APP_FOC_STATE_CALIB) {
        (void)app_motor_identify_fast_step();
    }

    /* 角度 */
    if (s_angle_src == APP_FOC_ANGLE_FORCED) {
        theta_e = foc_wrap_2pi(s_forced_theta);
        omega_e = 0.0f;
    } else {
        if (!app_foc_read_rotor_rad(&theta_m)) {
            app_foc_current_protect(); /* 换相数据停摆/无效：保护式零矢量 */
            return;
        }
        theta_e = s_angle.step(&s_angle, theta_m, s_dt_s, &omega_e);
    }

    /* 转矩模式限速（保护，不锁存）：|ωe| 超限 → 生效给定置零；带 15% 滞环恢复
     * （无滞环时 ωe 噪声/回摆会在阈值附近反复切断 → 机械顿挫、电流冲击）。
     * CALIB 由辨识模块直接给激励（calib_set_excitation），不受限速影响。 */
    if (s_state != APP_FOC_STATE_CALIB) {
        float speed_max = app_software_params_current()->control.limits.speed_max_rad_s;

        s_i_q_ref = s_i_q_ref_cmd;
        if (foc_finite(speed_max) && (speed_max > 0.0f)) {
            if (s_speed_limited) {
                if (fabsf(omega_e) < (speed_max * APP_FOC_SPEED_LIMIT_RESTORE)) {
                    s_speed_limited = false;
                }
            } else if (fabsf(omega_e) > speed_max) {
                s_speed_limited = true;
            }
            if (s_speed_limited) {
                s_i_q_ref = 0.0f;
            }
        } else {
            s_speed_limited = false;
        }
    }

    /* 电流环 */
    (void)app_foc_current_run(theta_e, omega_e, s_i_d_ref, s_i_q_ref, NULL, NULL);

    if ((s_state == APP_FOC_STATE_READY)
        && ((s_i_d_ref != 0.0f) || (s_i_q_ref_cmd != 0.0f))) {
        s_state = APP_FOC_STATE_RUN;
    }
}

void app_foc_run_once(void) {
    uint32_t t0 = intf_clock_get_cycle();

    s_dt_s = app_foc_measure_dt();
    g_foc_loop_dt_us = (uint32_t)(s_dt_s * 1000000.0f);
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

    /* 角度链参数重载：pole_pairs / direction / offset 改动在本次 foc on 生效
     * （pole_pairs 为 init 期消费，此处显式重建，避免必须重编译） */
    s_angle_ready = (app_foc_angle_init() == 0);
    if (!s_angle_ready) {
        return -1;
    }

    /* 注：app_3phase_inverter_enable() 内含 +12V 栅极供电稳定等待（约 10ms 阻塞）。
     * 该窗口内桥处于关闭态（PWM 未启动），无电流风险；但 25kHz 环与 L2/L3 保护暂停。
     * 与既有 V/F 启动路径同构；非阻塞桥使能为 v2 项（spec §10）。 */
    /* 清除调试 inv 可能遗留的逐相强制关断（否则该相输出被屏蔽） */
    for (uint8_t i = 0U; i < (uint8_t)APP_3PHASE_COUNT; i++) {
        (void)app_3phase_inverter_release((app_3phase_id_t)i);
    }
    if (app_3phase_inverter_enable() != 0) {
        return -1;
    }
    app_foc_current_reset();
    s_angle.reset(&s_angle);
    s_rotor_seq_valid = false; /* 重新建立采样序号基准 */
    s_angle_src = APP_FOC_ANGLE_ENCODER;
    s_i_d_ref = 0.0f;
    s_i_q_ref_cmd = 0.0f;
    s_i_q_ref = 0.0f;
    app_foc_current_zero_vector();
    s_state = APP_FOC_STATE_READY;
    return 0;
}

void app_foc_disable(void) {
    app_foc_current_zero_vector();
    app_3phase_inverter_disable();
    app_foc_current_reset(); /* 清除积分器与过流跳闸锁存 */
    s_i_d_ref = 0.0f;
    s_i_q_ref_cmd = 0.0f;
    s_i_q_ref = 0.0f;
    s_speed_limited = false; /* 限速滞环随禁用复位 */
    s_dt_valid = false;      /* 重新使能后首拍不注入 dt 尖峰 */
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
    s_i_q_ref_cmd = i_q_a;
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

float app_foc_get_iq_ref(void) { return s_i_q_ref_cmd; }

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
    s_i_q_ref_cmd = 0.0f;
    s_i_q_ref = 0.0f;
    s_state = APP_FOC_STATE_CALIB;
    return 0;
}

void app_foc_exit_calib(void) {
    if (s_state == APP_FOC_STATE_CALIB) {
        s_angle_src = APP_FOC_ANGLE_ENCODER;
        s_i_d_ref = 0.0f;
        s_i_q_ref_cmd = 0.0f;
        s_i_q_ref = 0.0f;
        s_angle.reset(&s_angle);
        s_state = APP_FOC_STATE_READY;
    }
}

void app_foc_calib_set_excitation(float theta_e_rad, float i_d_ref, float i_q_ref) {
    if (!foc_finite(theta_e_rad) || !foc_finite(i_d_ref) || !foc_finite(i_q_ref)) {
        return;
    }
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
