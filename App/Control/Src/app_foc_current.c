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
#include "app_analog_signal.h"
#include "app_hardware_params.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "foc_current.h"
#include "foc_math.h"
#include "foc_modulation.h"

#define APP_FOC_V_BUS_MIN_V (9.0f) /**< 最低母线电压 [V]（低于则拒绝输出，零矢量） */

static foc_current_t s_current;
static bool s_ready;

/* Ozone 观测（.noncacheable.bss：启动清零 + 调试器直读，不受 D-Cache 影响） */
app_foc_current_snapshot_t g_foc_current_snapshot
    __attribute__((section(".noncacheable.bss")));

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
    cfg.lambda = motor->ke_vs_per_rad;
    cfg.aw_decay = 0.99f;

    foc_current_ctor(&s_current);
    s_ready = (s_current.init(&s_current, &cfg) == 0);
    g_foc_current_snapshot = (app_foc_current_snapshot_t){0};
}

void app_foc_current_reset(void) {
    if (s_ready) {
        s_current.reset(&s_current);
    }
}

void app_foc_current_zero_vector(void) {
    (void)app_3phase_inverter_set_duty_abc(0.5f, 0.5f, 0.5f);
    g_foc_current_snapshot.duty_u = 0.5f;
    g_foc_current_snapshot.duty_v = 0.5f;
    g_foc_current_snapshot.duty_w = 0.5f;
    g_foc_current_snapshot.v_d_v = 0.0f;
    g_foc_current_snapshot.v_q_v = 0.0f;
    g_foc_current_snapshot.saturated = false;
    g_foc_current_snapshot.v_scale = 1.0f;
    g_foc_current_snapshot.valid = false; /* 保护路径：数据不可信（保持上一拍） */
    g_foc_current_snapshot.fault_count++;
}

int app_foc_current_run(float theta_e_rad, float omega_e_rad_s, float i_d_ref, float i_q_ref,
                        float duty_abc_out[3], bool* saturated_out) {
    const app_software_params_t* software = app_software_params_current();
    app_analog_values_t values;
    foc_current_in_t in = {0};
    foc_current_out_t out = {0};
    foc_modulation_cfg_t mod_cfg;
    float i_alpha, i_beta, v_alpha, v_beta, duty[3], v_scale = 1.0f, s, c;
    float v_bus, duty_max;

    if (!s_ready) {
        app_foc_current_zero_vector();
        return -1;
    }

    if (!app_analog_signal_read_all(&values)) {
        app_foc_current_zero_vector();
        return -1;
    }
    v_bus = values.v_bus_v;
    if (!foc_finite(v_bus) || (v_bus < APP_FOC_V_BUS_MIN_V)) {
        app_foc_current_zero_vector();
        return -1;
    }

    duty_max = software->control.limits.duty_max;

    /* 增益/前馈热更新（live 参数） */
    s_current.set_gains(&s_current, software->control.current_loop.kp,
                        software->control.current_loop.ki);
    s_current.set_decoupling(&s_current, software->control.current_loop.decoupling_en);

    /* Clarke → Park（同一 θ 的 sincos 复用于反 Park） */
    foc_clarke(values.i_u_a, values.i_v_a, values.i_w_a, &i_alpha, &i_beta);
    foc_sincos(theta_e_rad, &s, &c);
    foc_park_sc(i_alpha, i_beta, s, c, &in.i_d_a, &in.i_q_a);

    in.i_d_ref = i_d_ref;
    in.i_q_ref = i_q_ref;
    in.v_bus_v = v_bus;
    /* 圆形电压限幅：三相平衡时相电压峰值 A 对应占空比跨度 1.5·A/v_bus，
     * 故 A_max = (2·duty_max − 1)·v_bus / 1.5（与 foc_modulation 的 span 限幅一致） */
    in.v_max = (2.0f * duty_max - 1.0f) * v_bus / 1.5f;
    in.i_max = software->control.limits.i_q_max_a;
    in.omega_e_rad_s = omega_e_rad_s;

    if (s_current.step(&s_current, &in, &out) != 0) {
        app_foc_current_zero_vector();
        return -1;
    }

    /* 反 Park → 调制（min-max 零序注入） */
    foc_inv_park_sc(out.v_d, out.v_q, s, c, &v_alpha, &v_beta);
    mod_cfg.duty_max = duty_max;
    mod_cfg.v_bus_min = APP_FOC_V_BUS_MIN_V;
    if (foc_modulation_step(&mod_cfg, v_alpha, v_beta, v_bus, duty, &v_scale) != 0) {
        app_foc_current_zero_vector();
        return -1;
    }

    if (app_3phase_inverter_set_duty_abc(duty[0], duty[1], duty[2]) != 0) {
        app_foc_current_zero_vector();
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
    g_foc_current_snapshot.run_count++;
    return 0;
}

void app_foc_current_get_snapshot(app_foc_current_snapshot_t* out) {
    if (out != NULL) {
        *out = g_foc_current_snapshot;
    }
}
