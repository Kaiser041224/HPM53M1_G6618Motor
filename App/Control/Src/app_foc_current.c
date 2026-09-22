/**
 * @file    app_foc_current.c
 * @brief   FOC 电流环 25kHz 执行
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_foc_current.h"

#include "app_3phase_inverter.h"
#include "intf_clock.h"

#include "app_analog_signal.h"
#include "app_hardware_params.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "foc_current.h"
#include "foc_math.h"
#include "foc_modulation.h"

#include <math.h>

#define APP_FOC_V_BUS_MIN_V (9.0f) /**< 最低母线电压 [V]（低于则拒绝输出，零矢量） */

/* 控制层有效占空比上限（保守，不修改用户 config 的 duty_max=0.885）。
 * 目的：为四槽 PMT 低侧采样窗口留时序余量；0.70 为初步值，须示波确认后调整。
 * 对 PI 圆限幅（v_max）、调制（foc_modulation）与 vtest 一致生效。 */
#define APP_FOC_DUTY_MAX_CONSERVATIVE (0.70f)

/** 实际生效占空比上限（Ozone/调试可读；= min(user duty_max, 保守值)） */
volatile float g_foc_duty_max_effective __attribute__((section(".noncacheable.bss")));

static foc_current_t s_current;
/** 电流慢平均系数（25kHz → ~32Hz 一阶低通） */
#define APP_FOC_CURRENT_AVG_ALPHA (0.002f)

/** d/q 反馈慢平均状态 [A] */
static float s_i_d_avg;
static float s_i_q_avg;

static bool s_ready;
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
static float app_foc_effective_duty_max(const app_software_params_t* software) {
    float d = software->control.limits.duty_max;

    if (!foc_finite(d) || (d <= 0.5f)) {
        d = APP_FOC_DUTY_MAX_CONSERVATIVE;
    }
    if (d > APP_FOC_DUTY_MAX_CONSERVATIVE) {
        d = APP_FOC_DUTY_MAX_CONSERVATIVE;
    }
    g_foc_duty_max_effective = d;
    return d;
}

float app_foc_current_duty_max(void) { return g_foc_duty_max_effective; }

void app_foc_current_init(void) {
    const app_software_params_t* software = app_software_params_current();
    const app_motor_params_t* motor = app_motor_params_current();
    foc_current_cfg_t cfg;

    cfg.kp = software->control.current_loop.kp;
    cfg.ki = software->control.current_loop.ki;
    cfg.sample_time_s = 1.0f / (float)app_hardware_params_current()->inverter.pwm_freq_hz;
    cfg.decoupling_en = software->control.current_loop.decoupling_en;
    cfg.l_d = motor->ls_h;
    cfg.l_q = motor->ls_h;
    cfg.lambda = motor->flux_linkage_wb; /* 磁链（电角度·相峰值口径；勿用机械角·线 RMS 的 ke） */
    cfg.aw_decay = 0.99f;

    foc_current_ctor(&s_current);
    s_ready = (s_current.init(&s_current, &cfg) == 0);
    g_foc_current_snapshot = (app_foc_current_snapshot_t){0};
    (void)app_foc_effective_duty_max(software); /* 记录有效上限（init 即可观测） */
}

void app_foc_current_reset(void) {
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
}

int app_foc_current_vtest_start(float volts, float theta_e_rad, float duration_s) {
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

void app_foc_current_vtest_stop(void) {
    s_vtest_active = false;
    s_vtest_left_s = 0.0f;
}

bool app_foc_current_vtest_active(void) { return s_vtest_active; }

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
                s_tripped = true;
                g_foc_current_snapshot.tripped = true;
                app_foc_current_vtest_stop();
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

bool app_foc_current_is_tripped(void) { return s_tripped; }

bool app_foc_current_is_ready(void) { return s_ready; }

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
void app_foc_current_protect_reason(uint8_t reason) {
    if (reason < (uint8_t)APP_FOC_PROT_COUNT) {
        g_foc_protect_counts[reason]++;
    }
}

/**
 * @brief 保护式零矢量：输出零矢量 + 标记数据不可信 + 故障计数
 */
void app_foc_current_protect(void) {
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
    s_current.set_decoupling(&s_current, software->control.current_loop.decoupling_en);

    /* Clarke → Park（同一 θ 的 sincos 复用于反 Park） */
    foc_clarke(i_u_a, i_v_a, i_w_a, &i_alpha, &i_beta);
    foc_sincos(theta_e_rad, &s, &c);
    foc_park_sc(i_alpha, i_beta, s, c, &in.i_d_a, &in.i_q_a);

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
                s_tripped = true;
                g_foc_current_snapshot.tripped = true;
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
    in.i_max = software->control.limits.i_q_max_a;
    in.omega_e_rad_s = omega_e_rad_s;

    if (s_current.step(&s_current, &in, &out) != 0) {
        app_foc_current_protect_reason(APP_FOC_PROT_PI);
        app_foc_current_protect();
        return -1;
    }
    g_foc_cyc_pi = intf_clock_get_cycle() - t_pi;

    /* 反 Park → 调制（min-max 零序注入） */
    foc_inv_park_sc(out.v_d, out.v_q, s, c, &v_alpha, &v_beta);
    mod_cfg.duty_max = duty_max;
    mod_cfg.v_bus_min = APP_FOC_V_BUS_MIN_V;
    if (foc_modulation_step(&mod_cfg, v_alpha, v_beta, v_bus, duty, &v_scale) != 0) {
        app_foc_current_protect_reason(APP_FOC_PROT_MOD);
        app_foc_current_protect();
        return -1;
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
    g_foc_current_snapshot.i_d_a = foc_finite(in.i_d_a) ? in.i_d_a : 0.0f;
    g_foc_current_snapshot.i_q_a = foc_finite(in.i_q_a) ? in.i_q_a : 0.0f;
    /* 慢平均（~32Hz @25kHz）：终端单拍瞬时值无意义（PI 尚未响应） */
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
    if (g_foc_current_snapshot.valid) {
        g_foc_current_snapshot.run_count++;
    }
    app_foc_trace_capture();
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
        *out = g_foc_current_snapshot;
    }
}
