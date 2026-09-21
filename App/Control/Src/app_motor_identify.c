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

#define APP_IDENTIFY_I_CAL_A       (2.0f)   /**< 辨识电流（峰值）[A] */
#define APP_IDENTIFY_QUALITY_MIN   (0.8f)   /**< 质量下限（与 id_encoder 配置一致） */
#define APP_IDENTIFY_RUN_TICK_MAX  (30000U) /**< 编排侧 RUN 超时 [1kHz tick]（30s 兜底） */
#define APP_IDENTIFY_SETTLE_TICKS  (50U)    /**< 验证起始静默 [tick]（排除残余速度） */
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
static float s_prev_offset_rad;   /**< 辨识前运行态零点（验证失败回滚用） */
static float s_prev_direction;    /**< 辨识前运行态方向 */
static bool s_offset_applied;     /**< 辨识结果已应用到运行态（待验证/回滚） */
static uint32_t s_run_ticks;      /**< RUN 阶段 tick 计数（编排侧超时） */
static uint32_t s_verify_settle;  /**< 验证静默剩余 tick */
static uint32_t s_rotor_seq_last; /**< 转子采样序号（停摆检测） */
static bool s_rotor_seq_valid;    /**< 序号已建立 */

/**
 * @brief 读取转子机械角（未加软件零点）[rad]
 */
static bool identify_read_theta_m(float* theta_m_rad) {
    uint16_t raw;
    bool valid;

    {
        uint32_t seq;

        if ((app_encoder_get_rotor_raw(&raw, &valid, &seq) != 0) || !valid) {
            return false;
        }
        if (s_rotor_seq_valid && (seq == s_rotor_seq_last)) {
            return false; /* 采样停摆：拒绝陈旧角 */
        }
        s_rotor_seq_last = seq;
        s_rotor_seq_valid = true;
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
        /* 读取失败/无效/采样停摆：置失败原因（否则上层打印 reason=none） */
        s_result.fail_reason = APP_IDENTIFY_REASON_ENCODER;
        s_result.failed = true;
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
        const app_motor_params_t* motor = app_motor_params_current();

        s_result.offset_rad = out.offset_rad;
        s_result.direction = out.direction;
        s_result.quality = out.quality;
        s_result.ratio_err = out.mech_ratio_err;

        /* 记录辨识前运行态零点（验证失败时回滚，保证"失败保留原值"语义） */
        s_prev_offset_rad = motor->encoder.electrical_offset_rad;
        s_prev_direction = motor->encoder.direction;

        /* 先应用辨识结果（更新运行期角度链），再退出 CALIB 做闭环验证 */
        app_foc_apply_encoder_offset(out.offset_rad, out.direction);
        s_offset_applied = true;
        app_foc_exit_calib();
        (void)app_foc_set_id_ref(APP_IDENTIFY_I_CAL_A); /* i_q = 0 锁定验证 */

        s_state = APP_IDENTIFY_STATE_VERIFY;
        s_verify_ms = 0.0f;
        s_verify_sum_deg = 0.0f;
        s_verify_max_deg = 0.0f;
        s_verify_n = 0U;
    } else if (out.failed) {
        /* 直接映射 id_encoder 失败原因（不再由 quality 反推） */
        switch (out.fail_reason) {
        case ID_ENCODER_FAIL_TIMEOUT: s_result.fail_reason = APP_IDENTIFY_REASON_TIMEOUT; break;
        case ID_ENCODER_FAIL_DIR: s_result.fail_reason = APP_IDENTIFY_REASON_DIR; break;
        case ID_ENCODER_FAIL_QUALITY: s_result.fail_reason = APP_IDENTIFY_REASON_QUALITY; break;
        case ID_ENCODER_FAIL_RATIO: s_result.fail_reason = APP_IDENTIFY_REASON_RATIO; break;
        case ID_ENCODER_FAIL_NONFINITE: s_result.fail_reason = APP_IDENTIFY_REASON_NONFINITE; break;
        case ID_ENCODER_FAIL_CONFIG:
        default: s_result.fail_reason = APP_IDENTIFY_REASON_STATE; break;
        }
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
        /* FOC 必须仍处于 CALIB（被 disable 等异常退出 → 立即中止，防挂起） */
        if (app_foc_get_state() != APP_FOC_STATE_CALIB) {
            s_result.fail_reason = APP_IDENTIFY_REASON_STATE;
            s_result.failed = true;
            app_motor_identify_abort();
            return;
        }
        /* 编排侧超时兜底（30s @1kHz） */
        s_run_ticks++;
        if (s_run_ticks > APP_IDENTIFY_RUN_TICK_MAX) {
            s_result.fail_reason = APP_IDENTIFY_REASON_TIMEOUT;
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

        /* 验证窗口必须"活着"：FOC 状态、快照有效性、编码器健康 */
        if ((app_foc_get_state() != APP_FOC_STATE_READY)
            && (app_foc_get_state() != APP_FOC_STATE_RUN)) {
            s_result.fail_reason = (app_foc_get_state() == APP_FOC_STATE_FAULT)
                                       ? APP_IDENTIFY_REASON_FAULT
                                       : APP_IDENTIFY_REASON_STATE;
            s_result.failed = true;
            app_motor_identify_abort();
            return;
        }
        for (uint8_t i = 0U; i < (uint8_t)APP_ENCODER_COUNT; i++) {
            if (app_encoder_get_error_count((app_encoder_id_t)i) != s_enc_err_base[i]) {
                s_result.fail_reason = APP_IDENTIFY_REASON_ENCODER;
                s_result.failed = true;
                app_motor_identify_abort();
                return;
            }
        }

        app_foc_get_snapshot(&snap);
        if (!snap.valid) {
            s_result.fail_reason = APP_IDENTIFY_REASON_STATE; /* 保护路径：数据不可信 */
            s_result.failed = true;
            app_motor_identify_abort();
            return;
        }

        s_verify_ms += 1.0f; /* 1kHz */
        if (s_verify_settle > 0U) {
            s_verify_settle--; /* 起始静默：排除扫描残余速度 */
            return;
        }
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
                s_offset_applied = false; /* 已落库，无需回滚 */
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
    s_offset_applied = false;
    s_run_ticks = 0U;
    s_verify_settle = APP_IDENTIFY_SETTLE_TICKS;
    s_rotor_seq_valid = false;

    cfg.pole_pairs = motor->pole_pairs;
    cfg.i_cal_a = APP_IDENTIFY_I_CAL_A;
    cfg.lockin_ms = 500.0f;
    cfg.dir_ms = 300.0f;
    cfg.dir_step_rad = FOC_PI_F / 3.0f;
    cfg.sweep_steps = 180U;
    cfg.sweep_step_ms = 5.0f;
    cfg.quality_min = APP_IDENTIFY_QUALITY_MIN;
    cfg.ratio_tol = 0.2f;
    cfg.timeout_ms = 15000.0f;

    id_encoder_ctor(&s_id_encoder);
    if (s_id_encoder.init(&s_id_encoder, &cfg) != 0) {
        app_foc_exit_calib();
        s_result = (app_motor_identify_result_t){0};
        s_result.failed = true;
        s_result.fail_reason = APP_IDENTIFY_REASON_STATE; /* 配置非法：不留下陈旧结果 */
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
    if (s_offset_applied) {
        /* 验证未通过：运行态零点回滚到辨识前值（与"失败保留原值"语义一致） */
        app_foc_apply_encoder_offset(s_prev_offset_rad, s_prev_direction);
        s_offset_applied = false;
    }
    s_result.active = false;
    s_state = APP_IDENTIFY_STATE_IDLE;
}

/* 说明：结果字段（s_result）经 app_motor_identify_get_result() 供 Comm/Debug 读取；
 * Control 层不直接打印（分层：Comm → Control，避免 Control 依赖 Comm）。 */
