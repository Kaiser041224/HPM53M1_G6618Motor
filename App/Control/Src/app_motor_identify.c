/**
 * @file    app_motor_identify.c
 * @brief   电机辨识编排实现（V1：电角度辨识）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_motor_identify.h"

#include "app_encoder.h"
#include "app_fault.h"
#include "app_foc.h"
#include "app_hardware_params.h"
#include "app_motor_params.h"
#include "foc_math.h"
#include "id_encoder.h"

#define APP_IDENTIFY_I_CAL_A       (2.0f)  /**< 辨识电流（峰值）[A] */
#define APP_IDENTIFY_VERIFY_MS     (500.0f) /**< 验证时长 [ms] */
#define APP_IDENTIFY_VERIFY_MEAN_DEG (5.0f) /**< 验证残差均值上限 [deg] */
#define APP_IDENTIFY_VERIFY_MAX_DEG  (15.0f) /**< 验证残差峰值上限 [deg] */

typedef enum {
    APP_IDENTIFY_STATE_IDLE = 0,
    APP_IDENTIFY_STATE_RUN,    /**< id_encoder 运行中（25kHz 驱动） */
    APP_IDENTIFY_STATE_VERIFY, /**< 闭环验证中（1kHz 采样） */
    APP_IDENTIFY_STATE_DONE,
} app_identify_state_t;

static app_identify_state_t s_state;
static id_encoder_t s_id_encoder;
static uint32_t s_enc_err_base[APP_ENCODER_COUNT];
static float s_verify_ms;
static float s_verify_sum_deg;
static float s_verify_max_deg;
static uint32_t s_verify_n;
static app_motor_identify_result_t s_result;

/**
 * @brief 读取转子机械角（未加软件零点）[rad]
 */
static bool identify_read_theta_m(float* theta_m_rad) {
    uint16_t raw;
    bool valid;

    if ((app_encoder_get_rotor_raw(&raw, &valid, NULL) != 0) || !valid) {
        return false;
    }
    *theta_m_rad = (float)raw * (FOC_TWO_PI_F / 65536.0f);
    return true;
}

/**
 * @brief 读取编码器错误计数（健康检查）
 */
static void identify_read_errors(uint32_t* out) {
    for (uint8_t i = 0U; i < (uint8_t)APP_ENCODER_COUNT; i++) {
        out[i] = app_encoder_get_error_count((app_encoder_id_t)i);
    }
}

bool app_motor_identify_is_active(void) { return s_result.active; }

void app_motor_identify_get_result(app_motor_identify_result_t* out) {
    if (out != NULL) {
        *out = s_result;
    }
}

bool app_motor_identify_fast_step(void) {
    id_encoder_in_t in;
    id_encoder_out_t out;
    app_foc_current_snapshot_t snap;
    float theta_m;

    if (s_state != APP_IDENTIFY_STATE_RUN) {
        return false;
    }

    if (!identify_read_theta_m(&theta_m)) {
        app_motor_identify_abort();
        return false;
    }
    app_foc_get_snapshot(&snap);

    in.theta_m_raw_rad = theta_m;
    in.i_d_a = snap.i_d_a;
    in.i_q_a = snap.i_q_a;
    in.v_bus_v = snap.v_bus_v;
    in.dt_s = 1.0f / (float)app_hardware_params_current()->inverter.pwm_freq_hz;

    s_id_encoder.step(&s_id_encoder, &in, &out);
    app_foc_calib_set_excitation(out.theta_e_cmd, out.i_d_ref, out.i_q_ref);

    if (out.done) {
        s_result.offset_rad = out.offset_rad;
        s_result.direction = out.direction;
        s_result.quality = out.quality;
        s_result.ratio_err = out.mech_ratio_err;

        /* 先应用辨识结果（更新运行期角度链），再退出 CALIB 做闭环验证 */
        app_foc_apply_encoder_offset(out.offset_rad, out.direction);
        app_foc_exit_calib();
        (void)app_foc_set_id_ref(APP_IDENTIFY_I_CAL_A); /* i_q = 0 锁定验证 */

        s_state = APP_IDENTIFY_STATE_VERIFY;
        s_verify_ms = 0.0f;
        s_verify_sum_deg = 0.0f;
        s_verify_max_deg = 0.0f;
        s_verify_n = 0U;
    } else if (out.failed) {
        s_result.fail_reason = (out.quality < 0.8f) ? APP_IDENTIFY_REASON_QUALITY
                                                    : APP_IDENTIFY_REASON_RATIO;
        s_result.quality = out.quality;
        s_result.ratio_err = out.mech_ratio_err;
        s_result.failed = true;
        app_motor_identify_abort();
        return false;
    }
    return true;
}

void app_motor_identify_run_once(uint32_t now_ms) {
    (void)now_ms;

    switch (s_state) {
    case APP_IDENTIFY_STATE_RUN:
        /* FOC 因故障进入 FAULT：立即中止（否则 fast_step 停摆、辨识挂起） */
        if (app_foc_get_state() == APP_FOC_STATE_FAULT) {
            s_result.fail_reason = APP_IDENTIFY_REASON_FAULT;
            s_result.failed = true;
            app_motor_identify_abort();
            return;
        }
        /* 编码器健康：错误计数增量 > 0 → 中止 */
        for (uint8_t i = 0U; i < (uint8_t)APP_ENCODER_COUNT; i++) {
            if (app_encoder_get_error_count((app_encoder_id_t)i) != s_enc_err_base[i]) {
                s_result.fail_reason = APP_IDENTIFY_REASON_ENCODER;
                s_result.failed = true;
                app_motor_identify_abort();
                return;
            }
        }
        break;

    case APP_IDENTIFY_STATE_VERIFY: {
        app_foc_current_snapshot_t snap;
        float delta_deg;

        s_verify_ms += 1.0f; /* 1kHz */
        app_foc_get_snapshot(&snap);
        delta_deg = snap.theta_e_rad * (180.0f / FOC_PI_F);
        if (delta_deg > 180.0f) {
            delta_deg -= 360.0f;
        }
        s_verify_sum_deg += delta_deg;
        if (fabsf(delta_deg) > s_verify_max_deg) {
            s_verify_max_deg = fabsf(delta_deg);
        }
        s_verify_n++;

        if (s_verify_ms >= APP_IDENTIFY_VERIFY_MS) {
            float mean_deg = (s_verify_n > 0U) ? (s_verify_sum_deg / (float)s_verify_n) : 999.0f;

            s_result.verify_mean_deg = mean_deg;
            s_result.verify_max_deg = s_verify_max_deg;
            if ((fabsf(mean_deg) <= APP_IDENTIFY_VERIFY_MEAN_DEG)
                && (s_verify_max_deg <= APP_IDENTIFY_VERIFY_MAX_DEG)) {
                app_motor_params_t* motor = app_motor_params_mutable();

                motor->encoder.electrical_offset_rad = s_result.offset_rad;
                motor->encoder.direction = s_result.direction;
                s_result.done = true;
                s_result.active = false;
                s_state = APP_IDENTIFY_STATE_DONE;
            } else {
                s_result.fail_reason = APP_IDENTIFY_REASON_VERIFY;
                s_result.failed = true;
                app_motor_identify_abort();
            }
        }
        break;
    }

    case APP_IDENTIFY_STATE_DONE:
    case APP_IDENTIFY_STATE_IDLE:
    default:
        break;
    }
}

int app_motor_identify_start(void) {
    const app_motor_params_t* motor = app_motor_params_current();
    id_encoder_cfg_t cfg;

    if (s_state != APP_IDENTIFY_STATE_IDLE) {
        return -1;
    }
    if (app_fault_get_state() != APP_FAULT_STATE_NORMAL) {
        return -1;
    }
    if (app_foc_enter_calib() != 0) {
        return -1; /* 需 FOC 已使能（READY/RUN） */
    }

    identify_read_errors(s_enc_err_base);

    cfg.pole_pairs = motor->pole_pairs;
    cfg.i_cal_a = APP_IDENTIFY_I_CAL_A;
    cfg.lockin_ms = 500.0f;
    cfg.dir_ms = 300.0f;
    cfg.dir_step_rad = FOC_PI_F / 3.0f;
    cfg.sweep_steps = 180U;
    cfg.sweep_step_ms = 5.0f;
    cfg.quality_min = 0.8f;
    cfg.ratio_tol = 0.2f;
    cfg.timeout_ms = 15000.0f;

    id_encoder_ctor(&s_id_encoder);
    if (s_id_encoder.init(&s_id_encoder, &cfg) != 0) {
        app_foc_exit_calib();
        return -1;
    }
    s_id_encoder.reset(&s_id_encoder);
    s_result = (app_motor_identify_result_t){0};
    s_result.active = true;
    s_state = APP_IDENTIFY_STATE_RUN;
    return 0;
}

void app_motor_identify_abort(void) {
    if (s_state == APP_IDENTIFY_STATE_IDLE) {
        return;
    }
    (void)app_foc_set_id_ref(0.0f);
    (void)app_foc_set_iq_ref(0.0f);
    app_foc_exit_calib();
    s_result.active = false;
    s_state = APP_IDENTIFY_STATE_IDLE;
}

/* 说明：结果字段（s_result）经 app_motor_identify_get_result() 供 Comm/Debug 读取；
 * Control 层不直接打印（分层：Comm → Control，避免 Control 依赖 Comm）。 */
