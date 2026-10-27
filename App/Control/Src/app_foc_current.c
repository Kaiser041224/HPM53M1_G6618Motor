/**
 * @file    app_foc_current.c
 * @brief   FOC 电流环 30kHz 执行
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_fault.h"
#include "app_foc_current.h"
#include "app_foc.h"

#include "app_3phase_inverter.h"
#include "intf_clock.h"
#include "intf_sys.h"

#include "app_analog_signal.h"
#include "app_hardware_params.h"
#include "app_motor_params.h"
#include "app_ramfunc.h"
#include "app_software_params.h"
#include "foc_angle.h"
#include "foc_current.h"
#include "foc_math.h"
#include "foc_modulation.h"

#include <math.h>

#define APP_FOC_V_BUS_MIN_V (9.0f) /**< 最低母线电压 [V]（低于则拒绝输出，零矢量） */
/* 采样到下一 PWM 输出的一拍延迟；由 software.current_loop.output_delay_s 在 init 期覆盖。
 * 非法值退回 0（关闭），避免未标定延迟影响既有辨识/摩擦测试。 */
static float s_output_delay_s;
/* init 期缓存的参数单例地址：ISR 内读字段不再调用 Flash 访问器 */
static const app_motor_params_t* s_motor_params;

/* ── 逆变器死区补偿 ────────────────────────────────────────────────────
 * 死区+管压降造成的相电压畸变正比于 sign(i_phase)，幅值 Δduty = t_eq·f_pwm。
 * 按相电流极性在占空比上做常量偏移（符号带滞回，避免过零抖动）。
 * 量由 hardware.inverter.deadtime_comp_ns 折算（init/reload 期），0 = 关闭。 */
#define APP_FOC_DT_COMP_SIGN_THRESH_A (0.3f) /**< 相电流符号判定阈值 [A]（带内保持上一符号） */
#define APP_FOC_DT_COMP_DUTY_MAX      (0.02f) /**< 补偿占空比上限（2%；防误配置大幅偏移） */
static float s_dt_comp_duty;                 /**< 折算后的补偿占空比幅值 [duty] */
static int8_t s_i_sign_u;                    /**< U 相电流符号（-1/0/+1；带内保持） */
static int8_t s_i_sign_v;                    /**< V 相电流符号 */
static int8_t s_i_sign_w;                    /**< W 相电流符号 */
/** 死区补偿折算占空比（Ozone/终端观测；0 = 关闭） */
volatile float g_foc_dt_comp_duty __attribute__((section(".noncacheable.bss")));

void app_foc_current_set_output_delay(float delay_s) {
    if (foc_finite(delay_s) && (delay_s >= 0.0f) && (delay_s <= 1.0e-3f)) {
        s_output_delay_s = delay_s;
    }
}

/* 控制层有效占空比上限（与用户配置取较小值）。
 * 0.95为24V空载速度试验选定，采样精度仍须示波验证。
 * 对 PI 圆限幅（v_max）、调制（foc_modulation）与 vtest 一致生效。 */
#define APP_FOC_DUTY_MAX_DEFAULT (0.95f)
static float s_duty_ceiling = APP_FOC_DUTY_MAX_DEFAULT;
/** 实际上限（Ozone/调试器直读），受用户配置及运行时ceiling共同约束 */
volatile float g_foc_duty_max_effective __attribute__((section(".noncacheable.bss")));

int app_foc_current_set_duty_ceiling(float ceiling) {
    if (!foc_finite(ceiling) || ceiling<0.70f || ceiling>0.98f) return -1;
    s_duty_ceiling=ceiling;
    float requested=app_software_params_current()->control.limits.duty_max;
    g_foc_duty_max_effective=foc_finite(requested) && requested>0.5f
                               ? fminf(requested,ceiling) : fminf(APP_FOC_DUTY_MAX_DEFAULT,ceiling);
    return 0;
}

static foc_current_t s_current;
static bool s_ready;

/* 电流 boost 管理（FOC 内部限幅的一部分） */
static float s_i_limit_continuous_a;
static float s_i_limit_boost_a;
static float s_i_limit_boost_time_s;
static float s_i_limit_recover_time_s;
static float s_i_limit_recover_ratio;
static float s_i_limit_credit; /**< [0,1]，1 = boost 满额 */
static bool s_i_limit_ready;

static float s_i_limit_clamp01(float value) {
    return value > 1.0f ? 1.0f : value < 0.0f ? 0.0f : value;
}

/**
 * @brief 从 software 参数重载 boost 配置并重置预算
 */
static void foc_current_limit_init(void) {
    const app_software_params_t* params = app_software_params_current();

    s_i_limit_continuous_a = params->i_q_continuous_a;
    s_i_limit_boost_a = params->control.limits.i_q_max_a;
    s_i_limit_boost_time_s = params->i_q_boost_time_s;
    s_i_limit_recover_time_s = params->i_q_boost_recover_time_s;
    s_i_limit_recover_ratio = params->i_q_boost_recover_ratio;

    if (!foc_finite(s_i_limit_continuous_a) || (s_i_limit_continuous_a < 0.0f)) {
        s_i_limit_continuous_a = 0.0f;
    }
    if (!foc_finite(s_i_limit_boost_a) || (s_i_limit_boost_a < s_i_limit_continuous_a)) {
        s_i_limit_boost_a = s_i_limit_continuous_a;
    }
    if (!foc_finite(s_i_limit_boost_time_s) || (s_i_limit_boost_time_s < 0.0f)) {
        s_i_limit_boost_time_s = 0.0f;
    }
    if (!foc_finite(s_i_limit_recover_time_s) || (s_i_limit_recover_time_s < 0.0f)) {
        s_i_limit_recover_time_s = 0.0f;
    }
    if (!foc_finite(s_i_limit_recover_ratio) || (s_i_limit_recover_ratio < 0.0f)) {
        s_i_limit_recover_ratio = 0.0f;
    } else if (s_i_limit_recover_ratio > 1.0f) {
        s_i_limit_recover_ratio = 1.0f;
    }

    s_i_limit_credit = 1.0f;
    s_i_limit_ready = true;
}

/* cal current 激活时由 30kHz ISR 写入；保持 DLM，避免标定期 ISR 访问 AHB。 */
static id_rl_sample_t s_identify_samples[APP_FOC_IDENTIFY_SAMPLES];
static volatile uint16_t s_identify_count;
static bool s_identify_capture_active;
static bool s_identify_voltage_mode;
static float s_identify_voltage;
static float s_identify_previous_voltage;
static bool s_identify_trial_active;
static bool s_identify_r_ff_override_active;
static bool s_identify_r_ff_override;
static float s_identify_kp, s_identify_ki;
static uint32_t s_identify_voltage_cycles;
static float s_identify_bias_voltage;
static uint16_t s_identify_half_period;
static float s_identify_amplitude;

int app_foc_current_identify_voltage(float voltage, uint32_t cycles) {
    if (!s_ready || !foc_finite(voltage) || (voltage >= 0 && cycles == 0)) return -1;
    s_identify_bias_voltage = voltage;
    s_identify_voltage_cycles = voltage >= 0 ? cycles : 0;
    s_current.reset(&s_current);
    return 0;
}

int app_foc_current_identify_r_ff(uint8_t enable) {
    if (!s_ready) {
        return -1;
    }
    s_identify_r_ff_override_active = true;
    s_identify_r_ff_override = (enable != 0U) ? true : false;
    return 0;
}

int app_foc_current_identify_arm(float voltage, bool voltage_mode) {
    if (!s_ready || s_identify_capture_active || !foc_finite(voltage)) return -1;
    s_identify_count = 0;
    s_identify_half_period = 0;
    s_identify_previous_voltage = g_foc_current_snapshot.v_d_v;
    s_identify_voltage = voltage;
    s_identify_voltage_mode = voltage_mode;
    if (voltage_mode) s_current.reset(&s_current);
    s_identify_capture_active = true;
    return 0;
}
int app_foc_current_identify_pulse(float amplitude, uint16_t half_period) {
    if (!foc_finite(amplitude) || amplitude<=0 || half_period<4 || half_period>16
        || !s_identify_voltage_cycles) return -1;
    int rc=app_foc_current_identify_arm(s_identify_bias_voltage,true);
    if (rc) return rc;
    s_identify_half_period=half_period;
    s_identify_amplitude=amplitude;
    return 0;
}
int app_foc_current_identify_pi(float kp, float ki) {
    if (!s_ready || !foc_finite(kp) || !foc_finite(ki) || kp <= 0 || ki <= 0) return -1;
    s_current.reset(&s_current);
    s_identify_kp = kp; s_identify_ki = ki;
    s_identify_trial_active = true;
    return 0;
}
const id_rl_sample_t* app_foc_current_identify_data(uint16_t* count) {
    *count = s_identify_count;
    return s_identify_samples;
}
/** 电流慢平均系数（30kHz 采样；慢观测用，非控制反馈） */
#define APP_FOC_CURRENT_AVG_ALPHA (0.002f)

/** d/q 反馈慢平均状态 [A] */
static float s_i_d_avg;
static float s_i_q_avg;

static uint8_t s_trip_count; /**< 连续超限拍数 */
static bool s_tripped;       /**< 跳闸锁存（reset 清除） */

/* 开环电压诊断（vtest）：主循环只写、ADC ISR 只读；volatile 保证发布顺序
 * （v/theta/left 先写，active 最后写；ISR 以 active 为门控） */
static volatile bool s_vtest_active;
static volatile float s_vtest_v;
static volatile float s_vtest_theta;
static volatile float s_vtest_left_s;

/* 波形捕获（trace） */
static app_foc_trace_sample_t s_trace[APP_FOC_TRACE_MAX];
static uint16_t s_trace_count;
static bool s_trace_armed;

/* Ozone 观测（.noncacheable.bss：启动清零 + 调试器直读，不受 D-Cache 影响） */
app_foc_current_snapshot_t g_foc_current_snapshot
    __attribute__((section(".noncacheable.bss")));

/**
 * @brief 计算并记录控制层有效占空比上限（min(user, 保守值)；非法回退保守值）
 * @param software 软件参数
 * @return 有效占空比上限（>0.5）
 */
APP_ATTR_RAMFUNC
static float app_foc_effective_duty_max(const app_software_params_t* software) {
    float d = software->control.limits.duty_max;

    if (!foc_finite(d) || (d <= 0.5f)) {
        d = APP_FOC_DUTY_MAX_DEFAULT;
    }
    if (d > s_duty_ceiling) {
        d = s_duty_ceiling;
    }
    g_foc_duty_max_effective = d;
    return d;
}

float app_foc_current_duty_max(void) { return g_foc_duty_max_effective; }

/**
 * @brief 相电流符号更新（滞回：阈值带内保持上一符号，避免过零抖动）
 */
APP_ATTR_RAMFUNC
static int8_t foc_dt_comp_sign(int8_t prev, float i_a) {
    if (i_a > APP_FOC_DT_COMP_SIGN_THRESH_A) {
        return 1;
    }
    if (i_a < -APP_FOC_DT_COMP_SIGN_THRESH_A) {
        return -1;
    }
    return prev;
}

/**
 * @brief 从 hardware 参数折算死区补偿占空比（init 与 reload_model 均调用；
 *        param set + foc off/on 即可热更改，无需重烧）
 */
static void foc_dt_comp_reload(void) {
    const app_hardware_inverter_t* inverter = &app_hardware_params_current()->inverter;
    float duty = (float)inverter->deadtime_comp_ns * 1.0e-9f * (float)inverter->pwm_freq_hz;

    if (!foc_finite(duty) || (duty < 0.0f)) {
        duty = 0.0f;
    } else if (duty > APP_FOC_DT_COMP_DUTY_MAX) {
        duty = APP_FOC_DT_COMP_DUTY_MAX;
    }
    s_dt_comp_duty = duty;
    g_foc_dt_comp_duty = duty;
    s_i_sign_u = 0;
    s_i_sign_v = 0;
    s_i_sign_w = 0;
}

float app_foc_current_dt_comp_duty(void) { return g_foc_dt_comp_duty; }

void app_foc_current_init(void) {
    const app_software_params_t* software = app_software_params_current();
    const app_motor_params_t* motor = app_motor_params_current();
    foc_current_cfg_t cfg;

    s_motor_params = motor;
    foc_current_limit_init();

    cfg.kp = software->control.current_loop.kp;
    cfg.ki = software->control.current_loop.ki;
    cfg.sample_time_s = 1.0f / (float)app_hardware_params_current()->inverter.pwm_freq_hz;
    cfg.decoupling_en = software->control.current_loop.decoupling_en;
    cfg.l_d = motor->ls_h;
    cfg.l_q = motor->ls_h;
    /* bEMF 与转矩共用 Kt_peak 口径：λ = Kt_peak/(1.5·p)。
     * 不直接读 motor.flux_linkage_wb，避免旧 MPRM 中 λ/Kt 双源不一致。 */
    cfg.lambda = (motor->pole_pairs > 0U && foc_finite(motor->kt_nm_per_a)
                  && (motor->kt_nm_per_a > 0.0f))
                     ? motor->kt_nm_per_a / (1.5f * (float)motor->pole_pairs)
                     : 0.0f;
    cfg.r_ohm = (foc_finite(motor->rs_ohm) && (motor->rs_ohm > 0.0f)) ? motor->rs_ohm : 0.0f;
    cfg.r_ff_en = (software->current_ff.r_en != 0U && cfg.r_ohm > 0.0f) ? 1U : 0U;
    cfg.wl_ff_en = software->current_ff.wl_en;
    cfg.bemf_ff_en = software->current_ff.bemf_en;
    cfg.aw_decay = 0.99f;

    foc_current_ctor(&s_current);
    s_ready = (s_current.init(&s_current, &cfg) == 0);
    foc_dt_comp_reload();
    /* 输出延迟：YAML init 期消费；非法值退回 0（关闭），不阻断电流环 */
    s_output_delay_s = 0.0f;
    {
        float delay_s = software->current_loop_output_delay_s;

        if (foc_finite(delay_s) && (delay_s >= 0.0f) && (delay_s <= 1.0e-3f)) {
            s_output_delay_s = delay_s;
        }
    }
    g_foc_current_snapshot = (app_foc_current_snapshot_t){0};
    (void)app_foc_effective_duty_max(software); /* 记录有效上限（init 即可观测） */
}

int app_foc_current_reload_model(void) {
    const app_software_params_t* software = app_software_params_current();
    const app_motor_params_t* motor = app_motor_params_current();
    float r_ohm;
    float l_h;
    float lambda;

    if (!s_ready) {
        return -1;
    }
    r_ohm = motor->rs_ohm;
    l_h = motor->ls_h;
    if (!foc_finite(r_ohm) || (r_ohm < 0.0f) || !foc_finite(l_h) || (l_h < 0.0f)
        || !foc_finite(motor->kt_nm_per_a) || (motor->kt_nm_per_a < 0.0f)) {
        return -1;
    }
    lambda = (motor->pole_pairs > 0U && (motor->kt_nm_per_a > 0.0f))
                 ? motor->kt_nm_per_a / (1.5f * (float)motor->pole_pairs)
                 : 0.0f;
    if (s_current.set_model == NULL
        || s_current.set_model(&s_current, r_ohm, l_h, l_h, lambda) != 0) {
        return -1;
    }
    /* 死区补偿量同模型一起在 foc on 期重载（param set 后 foc off/on 生效） */
    foc_dt_comp_reload();
    s_motor_params = motor;
    s_current.set_gains(&s_current, software->control.current_loop.kp,
                        software->control.current_loop.ki);
    s_current.set_decoupling(&s_current, software->control.current_loop.decoupling_en);
    s_current.set_wl_ff(&s_current,
                        (software->control.current_loop.decoupling_en != 0U
                         || software->current_ff.wl_en != 0U) ? 1U : 0U);
    s_current.set_bemf_ff(&s_current,
                          (software->control.current_loop.decoupling_en != 0U
                           || software->current_ff.bemf_en != 0U) ? 1U : 0U);
    s_current.set_r_ff(&s_current, (software->current_ff.r_en != 0U) ? 1U : 0U);
    return 0;
}

void app_foc_current_limit_update(float current_a, float dt_s) {
    if (!s_i_limit_ready || !foc_finite(current_a) || !foc_finite(dt_s) || (dt_s <= 0.0f)) {
        return;
    }
    if (current_a < 0.0f) {
        current_a = -current_a;
    }
    if ((s_i_limit_boost_time_s <= 0.0f) || (s_i_limit_boost_a <= s_i_limit_continuous_a)) {
        return;
    }

    if (current_a > s_i_limit_continuous_a) {
        float denominator =
            s_i_limit_boost_a * s_i_limit_boost_a - s_i_limit_continuous_a * s_i_limit_continuous_a;

        if (denominator > 0.0f) {
            float excess =
                (current_a * current_a - s_i_limit_continuous_a * s_i_limit_continuous_a)
                / denominator;

            if (excess > 0.0f) {
                s_i_limit_credit -= (dt_s / s_i_limit_boost_time_s) * excess;
                s_i_limit_credit = s_i_limit_clamp01(s_i_limit_credit);
            }
        }
    } else if ((s_i_limit_recover_time_s > 0.0f)
               && (current_a <= s_i_limit_continuous_a * s_i_limit_recover_ratio)) {
        s_i_limit_credit += dt_s / s_i_limit_recover_time_s;
        s_i_limit_credit = s_i_limit_clamp01(s_i_limit_credit);
    }
}

APP_ATTR_RAMFUNC
float app_foc_current_get_i_limit(void) {
    /* 保护动作层的 DERATE 倍率在此乘入 → 经既有路径自动传播到 FOC 电流矢量限幅
     * 与 motion 转矩上限（两者都读本函数），无需各自改动。无 DERATE 时 = 1.0。 */
    float scale;

    if (!s_i_limit_ready) {
        return 0.0f;
    }
    scale = app_fault_get_limit_scale();
    if ((s_i_limit_boost_time_s > 0.0f) && (s_i_limit_boost_a > s_i_limit_continuous_a)
        && (s_i_limit_credit > 0.0f)) {
        return s_i_limit_boost_a * scale;
    }
    return s_i_limit_continuous_a * scale;
}

void app_foc_current_reset(void) {
    s_identify_voltage_cycles = 0;
    s_identify_half_period = 0;
    s_identify_capture_active = false;
    s_identify_voltage_mode = false;
    s_identify_trial_active = false;
    s_identify_r_ff_override_active = false;
    s_identify_r_ff_override = false;
    s_identify_count = 0;
    if (s_ready) {
        s_current.reset(&s_current);
    }
    s_trip_count = 0U;
    s_tripped = false;
    g_foc_current_snapshot.tripped = false;
    s_vtest_active = false;
    s_vtest_left_s = 0.0f;
    s_i_d_avg = 0.0f; /* 慢平均清零：避免上次运行的陈旧值 */
    s_i_q_avg = 0.0f;
    s_i_sign_u = 0; /* 相电流符号状态清零（补偿幅值不变，仅符号重判） */
    s_i_sign_v = 0;
    s_i_sign_w = 0;
}

int app_foc_current_vtest_start(float volts, float theta_e_rad, float duration_s) {
    if (app_foc_stop_pending() || app_foc_get_state()==APP_FOC_STATE_CALIB
        || app_foc_get_current_source()==APP_FOC_CURRENT_SOURCE_MOTION) return -1;
    if (!s_ready || !foc_finite(volts) || !foc_finite(theta_e_rad) || !foc_finite(duration_s)) {
        return -1;
    }
    if (volts < 0.0f) {
        volts = 0.0f;
    } else if (volts > 2.0f) {
        volts = 2.0f; /* 诊断限幅：I ≈ v/R，2V/0.158Ω ≈ 12.7A（跳闸兜底） */
    }
    if (duration_s < 0.05f) {
        duration_s = 0.05f;
    } else if (duration_s > 5.0f) {
        duration_s = 5.0f;
    }
    s_vtest_v = volts;
    s_vtest_theta = theta_e_rad;
    s_vtest_left_s = duration_s;
    s_vtest_active = true;
    return 0;
}

APP_ATTR_RAMFUNC
void app_foc_current_vtest_stop(void) {
    s_vtest_active = false;
    s_vtest_left_s = 0.0f;
}

APP_ATTR_RAMFUNC
bool app_foc_current_vtest_active(void) { return s_vtest_active; }

APP_ATTR_RAMFUNC /* ADC0 ISR（vtest 独占拍） */
int app_foc_current_vtest_step_fresh(float i_u_a, float i_v_a, float i_w_a, float v_bus_v,
                                     float dt_s) {
    const app_software_params_t* software = app_software_params_current();
    foc_modulation_cfg_t mod_cfg;
    float s, c, va, vb, duty[3];
    float v_bus = v_bus_v;
    float dt = (foc_finite(dt_s) && (dt_s > 0.0f) && (dt_s <= 5.0e-3f))
                   ? dt_s
                   : (1.0f / (float)app_hardware_params_current()->inverter.pwm_freq_hz);

    if (!s_vtest_active) {
        return -1;
    }

    s_vtest_left_s -= dt;
    if (!foc_finite(v_bus) || (v_bus < APP_FOC_V_BUS_MIN_V) || (s_vtest_left_s <= 0.0f)) {
        app_foc_current_vtest_stop();
        app_foc_current_zero_vector();
        return -1;
    }

    foc_sincos(s_vtest_theta, &s, &c);
    va = s_vtest_v * c;
    vb = s_vtest_v * s;
    mod_cfg.duty_max = app_foc_effective_duty_max(software);
    mod_cfg.v_bus_min = APP_FOC_V_BUS_MIN_V;
    if (foc_modulation_step(&mod_cfg, va, vb, v_bus, duty, NULL) != 0) {
        app_foc_current_vtest_stop();
        app_foc_current_zero_vector();
        return -1;
    }
    (void)app_3phase_inverter_set_duty_abc(duty[0], duty[1], duty[2]);

    /* 快照：命令电压 + 按命令角度换算的 d/q 测量（供符号/映射/标度核对） */
    {
        float i_alpha, i_beta;

        foc_clarke(i_u_a, i_v_a, i_w_a, &i_alpha, &i_beta);
        foc_park_sc(i_alpha, i_beta, s, c, &g_foc_current_snapshot.i_d_a,
                    &g_foc_current_snapshot.i_q_a);
        g_foc_current_snapshot.theta_e_rad = s_vtest_theta;
        g_foc_current_snapshot.omega_e_rad_s = 0.0f;
        g_foc_current_snapshot.i_d_ref_a = 0.0f;
        g_foc_current_snapshot.i_q_ref_a = 0.0f;
        g_foc_current_snapshot.v_d_v = s_vtest_v;
        g_foc_current_snapshot.v_q_v = 0.0f;
        g_foc_current_snapshot.v_bus_v = v_bus;
        g_foc_current_snapshot.duty_u = duty[0];
        g_foc_current_snapshot.duty_v = duty[1];
        g_foc_current_snapshot.duty_w = duty[2];
        g_foc_current_snapshot.v_scale = 1.0f;
        g_foc_current_snapshot.saturated = false;
        g_foc_current_snapshot.valid = true;
        g_foc_current_snapshot.run_count++;

        /* 过流保护（与电流环一致）：|i_dq| 超限 → 跳闸停机 */
        {
            float i_trip = software->control.limits.i_trip_a;
            float mag2 = (g_foc_current_snapshot.i_d_a * g_foc_current_snapshot.i_d_a)
                         + (g_foc_current_snapshot.i_q_a * g_foc_current_snapshot.i_q_a);

            if (foc_finite(i_trip) && (i_trip > 0.0f) && (mag2 > (i_trip * i_trip))) {
                if (app_fault_actions_enabled()) {
                    s_tripped = true; /* 跳闸锁存 + 停 vtest：M1 只判断不动作 */
                    g_foc_current_snapshot.tripped = true;
                    app_foc_current_vtest_stop();
                }
                app_foc_current_protect();
                return -1;
            }
        }
    }
    return 0;
}

/**
 * @brief vtest 单拍执行（主循环兼容包装：读模拟量缓存 + 标称 dt）
 */
int app_foc_current_vtest_step(void) {
    app_analog_values_t values;

    if (!s_vtest_active) {
        return -1;
    }
    if (!app_analog_signal_read_all(&values)) {
        app_foc_current_vtest_stop();
        app_foc_current_zero_vector();
        return -1;
    }
    return app_foc_current_vtest_step_fresh(values.i_u_a, values.i_v_a, values.i_w_a,
                                            values.v_bus_v,
                                            1.0f / (float)app_hardware_params_current()
                                                       ->inverter.pwm_freq_hz);
}

int app_foc_trace_arm(void) {
    if (!s_ready) {
        return -1;
    }
    s_trace_count = 0U;
    s_trace_armed = true;
    return 0;
}

const app_foc_trace_sample_t* app_foc_trace_data(uint16_t* count) {
    if (count != NULL) {
        *count = s_trace_count;
    }
    return s_trace;
}

void app_foc_trace_reset(void) {
    s_trace_count = 0U;
    s_trace_armed = false;
}

/**
 * @brief 捕获单拍样本（trace 用）
 */
APP_ATTR_RAMFUNC
static void app_foc_trace_capture(void) {
    if (!s_trace_armed) {
        return;
    }
    if (s_trace_count < APP_FOC_TRACE_MAX) {
        s_trace[s_trace_count].i_d_a = g_foc_current_snapshot.i_d_a;
        s_trace[s_trace_count].i_q_a = g_foc_current_snapshot.i_q_a;
        s_trace[s_trace_count].v_d_v = g_foc_current_snapshot.v_d_v;
        s_trace[s_trace_count].v_q_v = g_foc_current_snapshot.v_q_v;
        s_trace[s_trace_count].theta_e_rad = g_foc_current_snapshot.theta_e_rad;
        s_trace_count++;
    }
    if (s_trace_count >= APP_FOC_TRACE_MAX) {
        s_trace_armed = false;
    }
}

APP_ATTR_RAMFUNC
bool app_foc_current_is_tripped(void) { return s_tripped; }

bool app_foc_current_is_ready(void) { return s_ready; }

APP_ATTR_RAMFUNC
void app_foc_current_zero_vector(void) {
    (void)app_3phase_inverter_set_duty_abc(0.5f, 0.5f, 0.5f);
    g_foc_current_snapshot.duty_u = 0.5f;
    g_foc_current_snapshot.duty_v = 0.5f;
    g_foc_current_snapshot.duty_w = 0.5f;
    g_foc_current_snapshot.v_d_v = 0.0f;
    g_foc_current_snapshot.v_q_v = 0.0f;
    g_foc_current_snapshot.saturated = false;
    g_foc_current_snapshot.v_scale = 1.0f;
    /* 命令式零矢量（使能/关闭/待机）：不改变 valid/fault_count */
}

/** 保护路径分原因计数（.noncacheable.bss：Ozone/RTT 观测） */
volatile uint32_t g_foc_protect_counts[APP_FOC_PROT_COUNT]
    __attribute__((section(".noncacheable.bss")));

/**
 * @brief 记录一次保护路径并注明原因
 */
APP_ATTR_RAMFUNC
void app_foc_current_protect_reason(uint8_t reason) {
    if (reason < (uint8_t)APP_FOC_PROT_COUNT) {
        g_foc_protect_counts[reason]++;
    }
}

/**
 * @brief 保护式零矢量：输出零矢量 + 标记数据不可信 + 故障计数
 */
APP_ATTR_RAMFUNC
void app_foc_current_protect(void) {
    if (s_identify_capture_active) {
        /* 失败拍破坏等间隔采样契约：冻结为无效结果，禁止后续好帧拼接。 */
        s_identify_capture_active=false;
        s_identify_voltage_mode=false;
        s_identify_voltage_cycles=0;
        s_identify_samples[0].valid=false;
        s_identify_count=APP_FOC_IDENTIFY_SAMPLES;
    }
    app_foc_current_zero_vector();
    g_foc_current_snapshot.valid = false;
    g_foc_current_snapshot.fault_count++;
}

/* 分段耗时观测（Ozone/RTT；cycle） */
volatile uint32_t g_foc_cyc_read __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_foc_cyc_pi __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_foc_cyc_mod __attribute__((section(".noncacheable.bss")));

float app_foc_current_get_v_scale(void) {
    return g_foc_current_snapshot.v_scale;
}

/**
 * @brief 电流环（显式输入版本：供 ADC 完成回调使用，输入为本拍新鲜采样）
 * @note 与 app_foc_current_run 的区别：不读模拟量缓存，电流/母线由调用方给出
 */
APP_ATTR_RAMFUNC /* 30kHz 电流环主体（ADC0 PMT ISR） */
int app_foc_current_run_fresh(float theta_e_rad, float omega_e_rad_s, float i_d_ref, float i_q_ref,
                              float i_u_a, float i_v_a, float i_w_a, float v_bus_v,
                              float duty_abc_out[3], bool* saturated_out) {
    uint32_t t_entry = intf_clock_get_cycle();
    uint32_t t_pi = 0U;
    const app_software_params_t* software = app_software_params_current();
    foc_current_in_t in = {0};
    foc_current_out_t out = {0};
    foc_modulation_cfg_t mod_cfg;
    float i_alpha, i_beta, v_alpha, v_beta, duty[3], v_scale = 1.0f, s, c;
    float s_out, c_out, theta_e_out;
    float v_bus, duty_max;

    if (!s_ready) {
        app_foc_current_protect();
        return -1;
    }

    v_bus = v_bus_v;
    g_foc_cyc_read = intf_clock_get_cycle() - t_entry;
    if (!foc_finite(v_bus) || (v_bus < APP_FOC_V_BUS_MIN_V)) {
        app_foc_current_protect_reason(APP_FOC_PROT_VBUS);
        app_foc_current_protect();
        return -1;
    }

    duty_max = app_foc_effective_duty_max(software);
    t_pi = intf_clock_get_cycle();

    /* 增益/前馈热更新（live 参数） */
    s_current.set_gains(&s_current, software->control.current_loop.kp,
                        software->control.current_loop.ki);
    if (s_identify_trial_active)
        s_current.set_gains(&s_current, s_identify_kp, s_identify_ki);
    s_current.set_decoupling(&s_current, software->control.current_loop.decoupling_en);
    s_current.set_wl_ff(&s_current,
                        (software->control.current_loop.decoupling_en != 0U
                         || software->current_ff.wl_en != 0U) ? 1U : 0U);
    s_current.set_bemf_ff(&s_current,
                          (software->control.current_loop.decoupling_en != 0U
                           || software->current_ff.bemf_en != 0U) ? 1U : 0U);
    {
        /* 启用位只看用户开关；电阻值由 set_model 缓存，非法 param 不应把已验证模型
         * 的 R·I 前馈静默关掉（reload 失败时保留旧模型）。 */
        uint8_t r_ff = (software->current_ff.r_en != 0U) ? 1U : 0U;

        if (s_identify_r_ff_override_active) {
            r_ff = s_identify_r_ff_override ? 1U : 0U;
        }
        s_current.set_r_ff(&s_current, r_ff);
    }

    /* Clarke → Park（同一 θ 的 sincos 复用于反 Park） */
    foc_clarke(i_u_a, i_v_a, i_w_a, &i_alpha, &i_beta);
    foc_sincos(theta_e_rad, &s, &c);
    foc_park_sc(i_alpha, i_beta, s, c, &in.i_d_a, &in.i_q_a);

    if (!foc_finite(in.i_d_a) || !foc_finite(in.i_q_a)) {
        app_foc_current_protect();
        return -1;
    }

    /* 快速软件过流保护：|i_dq| 连续 2 拍超限 → 保护性停机（防止失控/母线跌落）。
     * 阈值 control.limits.i_trip_a（峰值口径，0 = 关闭）。 */
    {
        float i_trip = software->control.limits.i_trip_a;

        if (s_tripped) {
            app_foc_current_protect_reason(APP_FOC_PROT_TRIP);
            app_foc_current_protect();
            return -1;
        }
        if (foc_finite(i_trip) && (i_trip > 0.0f)
            && ((in.i_d_a * in.i_d_a + in.i_q_a * in.i_q_a) > (i_trip * i_trip))) {
            s_trip_count++;
            if (s_trip_count >= 2U) {
                if (app_fault_actions_enabled()) {
                    s_tripped = true; /* 跳闸锁存：M1 只判断不锁存 */
                    g_foc_current_snapshot.tripped = true;
                }
                app_foc_current_protect_reason(APP_FOC_PROT_TRIP);
                app_foc_current_protect();
                return -1;
            }
        } else {
            s_trip_count = 0U;
        }
    }

    in.i_d_ref = i_d_ref;
    in.i_q_ref = i_q_ref;
    in.v_bus_v = v_bus;
    /* 圆形电压限幅：与 foc_modulation 的 span 限幅严格一致
     * A_max = (2·duty_max − 1)·v_bus / √3（旧实现 /1.5 偏大 15.5%） */
    in.v_max = foc_modulation_vmax(duty_max, v_bus);
    /* 动态 boost 限幅 + live 的绝对上限取较小值（绝对上限可被调试/测试临时收紧）。 */
    in.i_max = app_foc_current_get_i_limit();
    {
        float hard_limit = software->control.limits.i_q_max_a;

        if (foc_finite(hard_limit) && (hard_limit >= 0.0f) && (hard_limit < in.i_max)) {
            in.i_max = hard_limit;
        }
    }
    in.omega_e_rad_s = omega_e_rad_s;

    bool identify_open = s_identify_voltage_cycles > 0
                         || (s_identify_capture_active && s_identify_voltage_mode);
    if (!identify_open
        && s_current.step(&s_current, &in, &out) != 0) {
        app_foc_current_protect_reason(APP_FOC_PROT_PI);
        app_foc_current_protect();
        return -1;
    }
    if (identify_open) {
        out.i_d_ref_lim = i_d_ref;
        out.i_q_ref_lim = i_q_ref;
        out.v_d = s_identify_capture_active && s_identify_voltage_mode
                      ? s_identify_voltage : s_identify_bias_voltage;
        if (s_identify_capture_active && s_identify_half_period)
            out.v_d=s_identify_bias_voltage
                + ((s_identify_count/s_identify_half_period)%2U ? -s_identify_amplitude
                                                               : s_identify_amplitude);
        out.v_q = 0;
        out.saturated = fabsf(out.v_d) > in.v_max;
        if (out.saturated) out.v_d = copysignf(in.v_max, out.v_d);
    }
    g_foc_cyc_pi = intf_clock_get_cycle() - t_pi;

    /* 反 Park 使用采样到输出的延迟补偿角；Park 仍用采样角 */
    theta_e_out = foc_angle_advance(theta_e_rad, omega_e_rad_s, s_output_delay_s);
    foc_sincos(theta_e_out, &s_out, &c_out);
    foc_inv_park_sc(out.v_d, out.v_q, s_out, c_out, &v_alpha, &v_beta);
    mod_cfg.duty_max = duty_max;
    mod_cfg.v_bus_min = APP_FOC_V_BUS_MIN_V;
    /* 死区补偿（>0 启用）：按本拍相电流极性注入每相占空比偏移。
     * 符号带内保持上一拍，避免过零抖动。
     * CALIB（cal rl / cal encoder / motor id）挂起：辨识守卫与 q 零偏基线
     * 按无补偿工厂标定——固定符号模式的补偿矢量会注入直流 q 轴电压，
     * 触发 Q_CURRENT 误判（实测 150ns 即偏移 ~0.8A）。
     * vtest/诊断路径不走此处。 */
    {
        int mod_rc;

        if ((s_dt_comp_duty > 0.0f) && (app_foc_get_state() != APP_FOC_STATE_CALIB)) {
            float duty_comp[3];

            s_i_sign_u = foc_dt_comp_sign(s_i_sign_u, i_u_a);
            s_i_sign_v = foc_dt_comp_sign(s_i_sign_v, i_v_a);
            s_i_sign_w = foc_dt_comp_sign(s_i_sign_w, i_w_a);
            duty_comp[0] = (float)s_i_sign_u * s_dt_comp_duty;
            duty_comp[1] = (float)s_i_sign_v * s_dt_comp_duty;
            duty_comp[2] = (float)s_i_sign_w * s_dt_comp_duty;
            mod_rc = foc_modulation_step_comp(&mod_cfg, v_alpha, v_beta, v_bus, duty_comp, duty,
                                              &v_scale);
        } else {
            mod_rc = foc_modulation_step(&mod_cfg, v_alpha, v_beta, v_bus, duty, &v_scale);
        }
        if (mod_rc != 0) {
            app_foc_current_protect_reason(APP_FOC_PROT_MOD);
            app_foc_current_protect();
            return -1;
        }
    }

    g_foc_cyc_mod = intf_clock_get_cycle() - t_pi - g_foc_cyc_pi;

    if (app_3phase_inverter_set_duty_abc(duty[0], duty[1], duty[2]) != 0) {
        app_foc_current_protect_reason(APP_FOC_PROT_DUTY);
        app_foc_current_protect();
        return -1;
    }

    if (duty_abc_out != NULL) {
        duty_abc_out[0] = duty[0];
        duty_abc_out[1] = duty[1];
        duty_abc_out[2] = duty[2];
    }
    if (saturated_out != NULL) {
        *saturated_out = out.saturated;
    }

    /* 反馈非有限：控制已按"反馈不可信"零电压处理，快照标记不可信并计故障 */
    {
        bool fb_ok = foc_finite(in.i_d_a) && foc_finite(in.i_q_a);

        g_foc_current_snapshot.valid = fb_ok;
        if (!fb_ok) {
            g_foc_current_snapshot.fault_count++;
        }
    }


    g_foc_current_snapshot.theta_e_rad = theta_e_rad;
    g_foc_current_snapshot.omega_e_rad_s = omega_e_rad_s;
    {
        float pp = (float)s_motor_params->pole_pairs;
        float dir = s_motor_params->encoder.direction;
        float red = s_motor_params->reducer_ratio;

        /* ωe 已经包含 encoder.direction；不能再除一次 dir，
         * 否则 direction=-1 时速度环反馈符号与位置/差分路径相反。 */
        g_foc_current_snapshot.speed_out_rad_s =
            (foc_finite(omega_e_rad_s) && (pp > 0.0f) && (red > 0.0f)
             && ((dir == 1.0f) || (dir == -1.0f)))
                ? omega_e_rad_s / (pp * red)
                : 0.0f;
        g_foc_current_snapshot.speed_out_valid = false; /* P0-B observer 有效时由 app_foc 置位 */
    }
    g_foc_current_snapshot.i_d_a = foc_finite(in.i_d_a) ? in.i_d_a : 0.0f;
    g_foc_current_snapshot.i_q_a = foc_finite(in.i_q_a) ? in.i_q_a : 0.0f;
    /* 慢平均（30kHz 采样）：终端单拍瞬时值无意义（PI 尚未响应） */
    s_i_d_avg += APP_FOC_CURRENT_AVG_ALPHA * (g_foc_current_snapshot.i_d_a - s_i_d_avg);
    s_i_q_avg += APP_FOC_CURRENT_AVG_ALPHA * (g_foc_current_snapshot.i_q_a - s_i_q_avg);
    g_foc_current_snapshot.i_d_avg_a = s_i_d_avg;
    g_foc_current_snapshot.i_q_avg_a = s_i_q_avg;
    g_foc_current_snapshot.i_d_ref_a = out.i_d_ref_lim;
    g_foc_current_snapshot.i_q_ref_a = out.i_q_ref_lim;
    g_foc_current_snapshot.v_d_v = out.v_d;
    g_foc_current_snapshot.v_q_v = out.v_q;
    g_foc_current_snapshot.duty_u = duty[0];
    g_foc_current_snapshot.duty_v = duty[1];
    g_foc_current_snapshot.duty_w = duty[2];
    g_foc_current_snapshot.v_bus_v = v_bus;
    g_foc_current_snapshot.v_scale = v_scale;
    g_foc_current_snapshot.saturated = out.saturated;
    g_foc_current_snapshot.voltage_max_v=in.v_max;
    g_foc_current_snapshot.voltage_utilization=
        foc_sqrtf(out.v_d*out.v_d+out.v_q*out.v_q)/in.v_max;
    if (g_foc_current_snapshot.valid && out.saturated)
        g_foc_current_snapshot.saturated_count++;
    if (g_foc_current_snapshot.valid) {
        g_foc_current_snapshot.run_count++;
    }
    app_foc_trace_capture();
    if (s_identify_voltage_cycles) --s_identify_voltage_cycles;
    if (s_identify_capture_active) {
        uint16_t index = s_identify_count;
        s_identify_samples[index] = (id_rl_sample_t){
            .i_d_a = in.i_d_a, .i_q_a = in.i_q_a,
            .i_u_a=i_u_a, .i_v_a=i_v_a, .i_w_a=i_w_a,
            .v_d_previous_v = s_identify_previous_voltage,
            .valid = g_foc_current_snapshot.valid && !out.saturated && v_scale >= 0.999f};
        /* 用本拍实际下发duty与母线重建下一拍配对电压，去除公共零序。 */
        float applied_alpha = v_bus * (2.0f*duty[0]-duty[1]-duty[2]) / 3.0f;
        float applied_beta = v_bus * (duty[1]-duty[2]) / FOC_SQRT3_F;
        s_identify_previous_voltage = applied_alpha*c + applied_beta*s;
        if (index + 1U == APP_FOC_IDENTIFY_SAMPLES) {
            s_identify_capture_active = false;
            s_identify_voltage_mode = false;
        }
        s_identify_count = index + 1U; /* 数据先写，计数最后发布 */
    }
    return 0;
}

int app_foc_current_run(float theta_e_rad, float omega_e_rad_s, float i_d_ref, float i_q_ref,
                        float duty_abc_out[3], bool* saturated_out) {
    app_analog_values_t values;

    if (!app_analog_signal_read_all(&values)) {
        app_foc_current_protect_reason(APP_FOC_PROT_READ);
        app_foc_current_protect();
        return -1;
    }
    return app_foc_current_run_fresh(theta_e_rad, omega_e_rad_s, i_d_ref, i_q_ref, values.i_u_a,
                                     values.i_v_a, values.i_w_a, values.v_bus_v, duty_abc_out,
                                     saturated_out);
}

void app_foc_current_get_snapshot(app_foc_current_snapshot_t* out) {
    if (out != NULL) {
        uint32_t state=intf_sys_irq_save();
        *out = g_foc_current_snapshot;
        intf_sys_irq_restore(state);
    }
}
