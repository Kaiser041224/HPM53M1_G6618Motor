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

#define APP_IDENTIFY_I_CAL_A       (2.0f)   /**< 辨识电流（峰值）[A]（4A 台架实测导致联轴打滑：残差 54~68°，回退 2A） */
#define APP_IDENTIFY_QUALITY_MIN   (0.8f)   /**< 质量下限（与 id_encoder 配置一致） */
#define APP_IDENTIFY_RUN_TICK_MAX  (60000U) /**< 编排侧 RUN 超时 [1kHz tick]（60s 兜底） */
/* 验证 = 探针法（参考实现同法）：静默后给一个小 i_q 脉冲，转子必须按"电角正方向"
 * 转动（机械行程 × direction ≥ 0.05 rad）。判据免疫机械回差/摩擦/爬行——只回答
 * "零点是否可用"：~90° 错误 → 无转矩 → 行程≈0 → 失败；180° 错误 → 反转 → 失败。
 * 静默段的角度残差仅作信息量（回差/摩擦会造成 ±7~14° 有界振荡，对 FOC 无影响），
 * 但残差峰值 > 90° 说明转子在"静止窗口"内大幅移动（飞转/零点严重错误）→ 一并失败。 */
#define APP_IDENTIFY_VERIFY_SETTLE_MS (50.0f) /**< 静默段时长 [ms]（排除残余速度） */
#define APP_IDENTIFY_VERIFY_RESID_MS  (50.0f) /**< 残差采样窗口 [ms] */
#define APP_IDENTIFY_VERIFY_RESID_MAX_DEG (90.0f) /**< 静默段残差峰值上限 [deg] */
#define APP_IDENTIFY_PROBE_A          (1.0f)  /**< 探针 i_q [A] */
#define APP_IDENTIFY_PROBE_MS         (60.0f) /**< 探针时长 [ms] */
#define APP_IDENTIFY_PROBE_MIN_RAD    (0.05f) /**< 探针最小机械行程 [rad] */

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
static float s_verify_first_deg; /**< 残差窗口首值 [deg]（漂移诊断） */
static bool s_verify_first_valid;
static app_motor_identify_result_t s_result;
static float s_prev_offset_rad;   /**< 辨识前运行态零点（验证失败回滚用） */
static float s_prev_direction;    /**< 辨识前运行态方向 */
static bool s_offset_applied;     /**< 辨识结果已应用到运行态（待验证/回滚） */
static uint32_t s_run_ticks;      /**< RUN 阶段 tick 计数（编排侧超时） */
static float s_probe_theta_prev;  /**< 探针段上一拍机械角 [rad] */
static float s_probe_travel;      /**< 探针段机械行程累计 [rad] */
static bool s_probe_active;       /**< 探针段已开始 */
static uint32_t s_rotor_seq_last; /**< 转子采样序号（停摆检测） */
static bool s_rotor_seq_valid;    /**< 序号已建立 */

/**
 * @brief 读取转子机械角（未加软件零点）[rad]
 */
static bool identify_read_theta_m(float* theta_m_rad) {
    bool valid;

    {
        uint32_t seq;

        if ((app_encoder_get_rotor_rad(theta_m_rad, &valid, &seq) != 0) || !valid) {
            return false;
        }
        if (s_rotor_seq_valid && (seq == s_rotor_seq_last)) {
            return false; /* 采样停摆：拒绝陈旧角 */
        }
        s_rotor_seq_last = seq;
        s_rotor_seq_valid = true;
    }
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
    /* 实测调用间隔：主循环节拍受 USB/终端/负载影响（台架实测可降到 ~4kHz），
     * 固定 1/25kHz 会让辨识时长膨胀数倍（曾 48s）；按真实 dt 推进保证时长可控 */
    in.dt_s = app_foc_get_last_dt_s();

    s_id_encoder.step(&s_id_encoder, &in, &out);
    app_foc_calib_set_excitation(out.theta_e_cmd, out.i_d_ref, out.i_q_ref);
    s_result.progress = out.progress; /* 终端心跳：长流程期间保持链路活跃 */

    if (out.done) {
        const app_motor_params_t* motor = app_motor_params_current();

        s_result.offset_rad = out.offset_rad;
        s_result.offset_fwd_rad = out.offset_fwd_rad;
        s_result.offset_rev_rad = out.offset_rev_rad;
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
        s_probe_active = false;
        s_probe_travel = 0.0f;
        s_verify_first_valid = false;
    } else if (out.failed) {
        /* 直接映射 id_encoder 失败原因（不再由 quality 反推） */
        switch (out.fail_reason) {
        case ID_ENCODER_FAIL_TIMEOUT: s_result.fail_reason = APP_IDENTIFY_REASON_TIMEOUT; break;
        case ID_ENCODER_FAIL_DIR: s_result.fail_reason = APP_IDENTIFY_REASON_DIR; break;
        case ID_ENCODER_FAIL_QUALITY: s_result.fail_reason = APP_IDENTIFY_REASON_QUALITY; break;
        case ID_ENCODER_FAIL_RATIO: s_result.fail_reason = APP_IDENTIFY_REASON_RATIO; break;
        case ID_ENCODER_FAIL_HYST: s_result.fail_reason = APP_IDENTIFY_REASON_HYST; break;
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

        /* 段 1：静默（排除扫描残余速度） */
        if (s_verify_ms <= APP_IDENTIFY_VERIFY_SETTLE_MS) {
            return;
        }

        /* 段 2：残差采样（i_d 锁定，转子静止；仅信息量） */
        if (s_verify_ms <= (APP_IDENTIFY_VERIFY_SETTLE_MS + APP_IDENTIFY_VERIFY_RESID_MS)) {
            delta_deg = snap.theta_e_rad * (180.0f / FOC_PI_F);
            if (delta_deg > 180.0f) {
                delta_deg -= 360.0f;
            }
            if (!s_verify_first_valid) {
                s_verify_first_deg = delta_deg;
                s_verify_first_valid = true;
            }
            s_verify_sum_deg += delta_deg;
            if (fabsf(delta_deg) > s_verify_max_deg) {
                s_verify_max_deg = fabsf(delta_deg);
            }
            s_verify_n++;
            return;
        }

        /* 探针段起点：解除 i_d 锁定，取起始机械角 */
        if (!s_probe_active) {
            if (!identify_read_theta_m(&s_probe_theta_prev)) {
                s_result.fail_reason = APP_IDENTIFY_REASON_ENCODER;
                s_result.failed = true;
                app_motor_identify_abort();
                return;
            }
            s_probe_travel = 0.0f;
            s_probe_active = true;
            (void)app_foc_set_id_ref(0.0f);
            (void)app_foc_set_iq_ref(APP_IDENTIFY_PROBE_A);
            return;
        }

        /* 探针段：累计机械行程（wrap-safe） */
        {
            float theta_m;

            if (identify_read_theta_m(&theta_m)) {
                s_probe_travel += foc_wrap_pm_pi(theta_m - s_probe_theta_prev);
                s_probe_theta_prev = theta_m;
            }
        }
        if (s_verify_ms
            < (APP_IDENTIFY_VERIFY_SETTLE_MS + APP_IDENTIFY_VERIFY_RESID_MS
               + APP_IDENTIFY_PROBE_MS)) {
            return;
        }

        /* 判定 */
        (void)app_foc_set_iq_ref(0.0f);
        s_result.verify_mean_deg = (s_verify_n > 0U) ? (s_verify_sum_deg / (float)s_verify_n) : 0.0f;
        s_result.verify_max_deg = s_verify_max_deg;
        /* 残差漂移 [deg/s]：静默窗口内转子被"恒转矩"驱动的直接证据（静止应为 ~0） */
        s_result.verify_drift_deg_s =
            (s_verify_first_valid && (s_verify_n > 1U))
                ? ((s_verify_sum_deg / (float)s_verify_n - s_verify_first_deg)
                   / (APP_IDENTIFY_VERIFY_RESID_MS / 1000.0f))
                : 0.0f;
        s_result.probe_travel_rad = s_probe_travel;
        if ((s_probe_travel * s_result.direction) >= APP_IDENTIFY_PROBE_MIN_RAD
            && (s_verify_max_deg <= APP_IDENTIFY_VERIFY_RESID_MAX_DEG)) {
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
    s_rotor_seq_valid = false;

    cfg.pole_pairs = motor->pole_pairs;
    cfg.i_cal_a = APP_IDENTIFY_I_CAL_A;
    cfg.lockin_ms = 800.0f;
    cfg.dir_ms = 400.0f;
    cfg.dir_step_rad = FOC_PI_F / 3.0f;
    cfg.sweep_steps = 1080U;    /* 3 电周期 × 360 步 */
    cfg.sweep_turns = 3.0f;     /* 多圈：平均局部传动误差（编码器/齿轮偏心） */
    cfg.sweep_step_ms = 8.0f;
    cfg.sweep_settle_ms = 200.0f;
    cfg.hyst_max_rad = 0.5f;    /* 正/反向零点差 > 28.6° 电角 → 判定回差/打滑 */
    cfg.quality_min = APP_IDENTIFY_QUALITY_MIN;
    cfg.ratio_tol = 0.2f;
    cfg.timeout_ms = 40000.0f;

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
