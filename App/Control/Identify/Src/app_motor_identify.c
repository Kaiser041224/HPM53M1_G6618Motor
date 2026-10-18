/**
 * @file    app_motor_identify.c
 * @brief   电角度自校准实现（隔离模块：1kHz 任务驱动、经 FOC 校准端口借用电流环）
 * @author  Kaiser
 *
 * 流程（单一入口 app_motor_identify_run_once，1kHz）：
 *   RUN    ：id_encoder 纯状态机（lock-in → 方向判定 → 正/反扫描 → 计算），
 *            每拍回喂编码器机械角，下发强制角 + i_d 激励（app_foc_calib_set_excitation）
 *   VERIFY ：试用零点（app_foc_calib_use_trial_offset，编码器角源）
 *            静默 50ms（i_d 锁定）→ 残差采样 50ms → i_q 探针 60ms（转子须沿正方向转动）
 *   结束   ：任何结果都经 app_foc_calib_end() 退出 CALIB（运行零点恢复，试用值作废）
 *
 * 每拍公共守卫：FOC 仍为 CALIB、故障模块非 FAULT、编码器错误/跳变计数不增长、
 *              |i_dq| 未持续超限、编排总时长未超限。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_motor_identify.h"

#include "app_encoder.h"
#include "app_fault.h"
#include "app_foc.h"
#include "app_motor_params.h"
#include "app_param.h"
#include "foc_math.h"
#include "id_encoder.h"
#include "intf_clock.h"

#include <math.h>
#include <string.h>

/* ---- 激励与安全 ---- */
#define APP_IDENTIFY_I_CAL_A (2.0f) /**< 辨识电流（d 轴峰值）[A]（4A 台架实测联轴打滑，回退 2A） \
                                     */
#define APP_IDENTIFY_I_ABORT_A    (6.0f)   /**< 过流中止阈值 |i_dq| [A]（= 3 × I_cal） */
#define APP_IDENTIFY_OC_TRIP_MS   (5U)     /**< 过流持续确认 [ms] */
#define APP_IDENTIFY_QUALITY_MIN  (0.8f)   /**< 质量下限 */
#define APP_IDENTIFY_RUN_TICK_MAX (60000U) /**< 编排总时长上限 [ms]（兜底；id_encoder 自身 40s） \
                                            */
#define APP_IDENTIFY_DT_NOMINAL_S (1.0e-3f) /**< 名义节拍 [s]（首拍/异常 dt 回退） */

/* ---- 验证（探针法）：零点 ~90° 错误 → 无转矩 → 行程≈0 → 失败；180° 错误 → 反转 → 失败 ---- */
#define APP_IDENTIFY_VERIFY_SETTLE_MS     (50U)   /**< 静默段 [ms]（排除扫描残余速度） */
#define APP_IDENTIFY_VERIFY_RESID_MS      (50U)   /**< 残差采样窗口 [ms] */
#define APP_IDENTIFY_VERIFY_RESID_MAX_DEG (15.0f) /**< 验证残差峰值上限 [deg 电角]：
                                                    * 带 50:1 减速器+输出负载时实测 12.85°；
                                                    * 10° 过紧。15° 仍拒绝 23°+ 的坏结果
                                                    * （44° 级必须重标，不得 apply）。 */
#define APP_IDENTIFY_PROBE_A              (1.0f)  /**< 探针 i_q [A] */
#define APP_IDENTIFY_PROBE_MS             (60U)   /**< 探针时长 [ms] */
#define APP_IDENTIFY_PROBE_MIN_RAD        (0.05f) /**< 探针最小机械行程 [rad] */

/* ---- 持久化记录（app_param 键 APP_PARAM_KEY_FOC_ENCODER） ---- */
#define APP_IDENTIFY_RECORD_VERSION (1U)

/**
 * @brief 电角度零点 flash 记录布局
 */
typedef struct {
    uint16_t version;    /**< 记录版本 */
    uint16_t pole_pairs; /**< 标定时极对数（恢复时须一致） */
    float offset_rad;    /**< 电角度零点 [rad] */
    float direction;     /**< 方向（±1） */
} app_identify_record_t;

_Static_assert(sizeof(app_identify_record_t) <= APP_PARAM_DATA_MAX, "record too large");

/**
 * @brief 编排状态
 */
typedef enum {
    APP_IDENTIFY_STATE_IDLE = 0,          /**< 空闲（run_once 为空操作） */
    APP_IDENTIFY_STATE_RUN,               /**< id_encoder 扫描中 */
    APP_IDENTIFY_STATE_VERIFY,            /**< 试用零点闭环验证中 */
} app_identify_state_t;

static app_identify_state_t s_identify_state;
static id_encoder_t s_id_encoder;
static app_motor_identify_result_t s_identify_result;
static bool s_identify_result_applied;    /**< 最近一次成功结果已 apply（save 前置条件） */

static uint32_t s_identify_last_cycle;    /**< 上一拍时刻 [cycle]（实测 dt） */
static float s_identify_sec_per_cycle;    /**< 1 / CPU 频率 [s/cycle] */
static uint32_t s_identify_ticks;         /**< 流程已执行拍数 [ms] */
static uint32_t s_identify_oc_ticks;      /**< 过流连续拍数 */
static uint32_t s_identify_enc_err_base;  /**< 编码器错误计数基线 */
static uint32_t s_identify_enc_jump_base; /**< 编码器跳变计数基线 */
static uint32_t s_identify_rotor_seq;     /**< 上一转子样本序号（停摆检测） */
static bool s_identify_rotor_seq_valid;   /**< 序号基线已建立 */

/* 验证段状态 */
static uint32_t s_verify_ms;
static float s_verify_sum_deg;
static float s_verify_max_deg;
static float s_verify_first_deg;
static uint32_t s_verify_count;
static bool s_verify_first_valid;
static bool s_probe_active;
static float s_probe_theta_prev;
static float s_probe_travel;

/**
 * @brief 结束流程（成功/失败共用出口）：经端口退出 CALIB，状态回 IDLE
 */
static void identify_finish(void) {
    s_identify_state = APP_IDENTIFY_STATE_IDLE;
    s_identify_result.active = false;
    s_identify_result.elapsed_ms = s_identify_ticks;
    app_foc_calib_end(); /* 运行零点恢复；非 CALIB 时无操作 */
}

/**
 * @brief 失败出口
 * @param reason 失败原因
 */
static void identify_fail(app_identify_fail_t reason) {
    s_identify_result.failed = true;
    s_identify_result.done = false;
    s_identify_result.fail_reason = reason;
    identify_finish();
}

/**
 * @brief 读取转子机械角（未加软件零点）；拒绝无效/停摆样本
 * @param theta_m_rad 输出机械角 [rad]
 * @return true = 有效新样本
 */
static bool identify_read_theta_m(float* theta_m_rad) {
    app_encoder_rotor_snapshot_t enc;

    if ((app_encoder_get_rotor_snapshot(&enc) != 0) || !enc.valid) {
        return false;
    }
    if (s_identify_rotor_seq_valid && (enc.seq == s_identify_rotor_seq)) {
        return false; /* 采样停摆（25kHz 采样下 1kHz 读取必有新样本） */
    }
    s_identify_rotor_seq = enc.seq;
    s_identify_rotor_seq_valid = true;
    *theta_m_rad = enc.rad;
    return true;
}

/**
 * @brief 实测本拍间隔 [s]（异常回退名义值）
 * @return dt [s]
 */
static float identify_measure_dt(void) {
    uint32_t now = intf_clock_get_cycle();
    float dt = (float)(now - s_identify_last_cycle) * s_identify_sec_per_cycle;

    s_identify_last_cycle = now;
    if (!(dt >= 1.0e-4f) || (dt > 5.0e-3f)) {
        dt = APP_IDENTIFY_DT_NOMINAL_S;
    }
    return dt;
}

/**
 * @brief 公共守卫（每拍）
 * @param snap 本拍 FOC 电流快照
 * @return true = 可继续；false = 已失败退出
 */
static bool identify_guard(const app_foc_current_snapshot_t* snap) {
    float i_mag2 = (snap->i_d_a * snap->i_d_a) + (snap->i_q_a * snap->i_q_a);
    float i_mag = foc_sqrtf(i_mag2);

    if (app_foc_get_state() != APP_FOC_STATE_CALIB) {
        identify_fail(APP_IDENTIFY_REASON_STATE);   /* 被 foc off / 故障等外部改变 */
        return false;
    }
    if (!snap->valid) {
        identify_fail(APP_IDENTIFY_REASON_STATE);   /* 电流环保护路径（母线/PI/调制）：数据不可信 */
        return false;
    }
    if (app_fault_get_state() == APP_FAULT_STATE_FAULT) {
        identify_fail(APP_IDENTIFY_REASON_FAULT);
        return false;
    }
    if ((app_encoder_get_error_count(APP_ENCODER_ROTOR) != s_identify_enc_err_base)
        || (app_encoder_get_rotor_jump_count() != s_identify_enc_jump_base)) {
        identify_fail(APP_IDENTIFY_REASON_ENCODER); /* 取帧失败 / 坏帧跳变：角度基准不可信 */
        return false;
    }
    if (foc_finite(i_mag) && (i_mag > s_identify_result.i_peak_a)) {
        s_identify_result.i_peak_a = i_mag;
    }
    if (!foc_finite(i_mag2) || (i_mag2 > (APP_IDENTIFY_I_ABORT_A * APP_IDENTIFY_I_ABORT_A))) {
        s_identify_oc_ticks++;
        if (s_identify_oc_ticks >= APP_IDENTIFY_OC_TRIP_MS) {
            identify_fail(APP_IDENTIFY_REASON_OVERCURRENT);
            return false;
        }
    } else {
        s_identify_oc_ticks = 0U;
    }
    if (s_identify_ticks > APP_IDENTIFY_RUN_TICK_MAX) {
        identify_fail(APP_IDENTIFY_REASON_TIMEOUT);
        return false;
    }
    return true;
}

/**
 * @brief id_encoder 失败原因映射
 * @param reason id_encoder 失败原因
 * @return 编排失败原因
 */
static app_identify_fail_t identify_map_fail(id_encoder_fail_t reason) {
    switch (reason) {
    case ID_ENCODER_FAIL_TIMEOUT: return APP_IDENTIFY_REASON_TIMEOUT;
    case ID_ENCODER_FAIL_DIR: return APP_IDENTIFY_REASON_DIR;
    case ID_ENCODER_FAIL_QUALITY: return APP_IDENTIFY_REASON_QUALITY;
    case ID_ENCODER_FAIL_RATIO: return APP_IDENTIFY_REASON_RATIO;
    case ID_ENCODER_FAIL_HYST: return APP_IDENTIFY_REASON_HYST;
    case ID_ENCODER_FAIL_NONFINITE: return APP_IDENTIFY_REASON_NONFINITE;
    case ID_ENCODER_FAIL_CONFIG:
    default: return APP_IDENTIFY_REASON_STATE;
    }
}

/**
 * @brief RUN 阶段单拍：回喂测量 → id_encoder → 下发激励 / 进入验证
 * @param snap 本拍 FOC 电流快照
 * @param dt_s 本拍间隔 [s]
 */
static void identify_step_run(const app_foc_current_snapshot_t* snap, float dt_s) {
    id_encoder_in_t in;
    id_encoder_out_t out;

    if (!identify_read_theta_m(&in.theta_m_raw_rad)) {
        identify_fail(APP_IDENTIFY_REASON_ENCODER);
        return;
    }
    in.i_d_a = snap->i_d_a;
    in.i_q_a = snap->i_q_a;
    in.v_bus_v = snap->v_bus_v;
    in.dt_s = dt_s;

    s_id_encoder.step(&s_id_encoder, &in, &out);
    s_identify_result.progress = out.progress * 0.95f;
    s_identify_result.quality = out.quality;
    s_identify_result.ratio_err = out.mech_ratio_err;

    if (out.failed) {
        identify_fail(identify_map_fail(out.fail_reason));
        return;
    }
    if (out.done) {
        s_identify_result.offset_rad = out.offset_rad;
        s_identify_result.offset_fwd_rad = out.offset_fwd_rad;
        s_identify_result.offset_rev_rad = out.offset_rev_rad;
        s_identify_result.direction = out.direction;

        /* 试用零点：编码器角源 + i_d 锁定（运行零点不动，end 时恢复） */
        if ((app_foc_calib_use_trial_offset(out.offset_rad, out.direction) != 0)
            || (app_foc_calib_set_current(APP_IDENTIFY_I_CAL_A, 0.0f) != 0)) {
            identify_fail(APP_IDENTIFY_REASON_STATE);
            return;
        }
        s_verify_ms = 0U;
        s_verify_sum_deg = 0.0f;
        s_verify_max_deg = 0.0f;
        s_verify_first_deg = 0.0f;
        s_verify_count = 0U;
        s_verify_first_valid = false;
        s_probe_active = false;
        s_probe_travel = 0.0f;
        s_identify_state = APP_IDENTIFY_STATE_VERIFY;
        return;
    }
    if (app_foc_calib_set_excitation(out.theta_e_cmd, out.i_d_ref, out.i_q_ref) != 0) {
        identify_fail(APP_IDENTIFY_REASON_STATE);
    }
}

/**
 * @brief VERIFY 阶段单拍：静默 → 残差采样 → 探针 → 判定
 * @param snap 本拍 FOC 电流快照
 */
static void identify_step_verify(const app_foc_current_snapshot_t* snap) {
    const uint32_t resid_end = APP_IDENTIFY_VERIFY_SETTLE_MS + APP_IDENTIFY_VERIFY_RESID_MS;
    const uint32_t probe_end = resid_end + APP_IDENTIFY_PROBE_MS;

    s_verify_ms++;
    s_identify_result.progress = 0.95f + 0.05f * ((float)s_verify_ms / (float)probe_end);

    /* 段 1：静默（i_d 锁定，排除扫描残余速度） */
    if (s_verify_ms <= APP_IDENTIFY_VERIFY_SETTLE_MS) {
        return;
    }

    /* 段 2：残差采样（零点正确 → 转子停在扫描终点 θe≈0；信息量 + 大偏移判据） */
    if (s_verify_ms <= resid_end) {
        float delta_deg = snap->theta_e_rad * (180.0f / FOC_PI_F);

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
        s_verify_count++;
        return;
    }

    /* 段 3：探针起点（解除 i_d 锁定，给 i_q） */
    if (!s_probe_active) {
        if (!identify_read_theta_m(&s_probe_theta_prev)) {
            identify_fail(APP_IDENTIFY_REASON_ENCODER);
            return;
        }
        s_probe_travel = 0.0f;
        s_probe_active = true;
        if (app_foc_calib_set_current(0.0f, APP_IDENTIFY_PROBE_A) != 0) {
            identify_fail(APP_IDENTIFY_REASON_STATE);
        }
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
    if (s_verify_ms < probe_end) {
        return;
    }

    /* 判定 */
    (void)app_foc_calib_set_current(0.0f, 0.0f);
    s_identify_result.verify_mean_deg =
        (s_verify_count > 0U) ? (s_verify_sum_deg / (float)s_verify_count) : 0.0f;
    s_identify_result.verify_max_deg = s_verify_max_deg;
    s_identify_result.verify_drift_deg_s =
        (s_verify_first_valid && (s_verify_count > 1U))
            ? ((s_identify_result.verify_mean_deg - s_verify_first_deg)
               / ((float)APP_IDENTIFY_VERIFY_RESID_MS / 1000.0f))
            : 0.0f;
    s_identify_result.probe_travel_rad = s_probe_travel;

    if (((s_probe_travel * s_identify_result.direction) >= APP_IDENTIFY_PROBE_MIN_RAD)
        && (s_verify_max_deg <= APP_IDENTIFY_VERIFY_RESID_MAX_DEG)) {
        s_identify_result.done = true;
        s_identify_result.failed = false;
        s_identify_result.fail_reason = APP_IDENTIFY_REASON_NONE;
        s_identify_result.progress = 1.0f;
        s_identify_result_applied = false; /* 新结果待 apply */
        identify_finish();
    } else {
        identify_fail(APP_IDENTIFY_REASON_VERIFY);
    }
}

bool app_motor_identify_is_active(void) { return s_identify_result.active; }

void app_motor_identify_get_result(app_motor_identify_result_t* out) {
    if (out != NULL) {
        *out = s_identify_result;
    }
}

int app_motor_identify_start(void) {
    const app_motor_params_t* motor = app_motor_params_current();
    id_encoder_cfg_t cfg;
    uint32_t cpu_hz = intf_clock_get_cpu_freq();

    if (s_identify_state != APP_IDENTIFY_STATE_IDLE) {
        return -1;
    }
    if (app_fault_get_state() == APP_FAULT_STATE_FAULT) {
        return -1;
    }
    if ((motor->pole_pairs == 0U) || (cpu_hz == 0U)) {
        return -1;
    }

    cfg.pole_pairs = motor->pole_pairs;
    cfg.i_cal_a = APP_IDENTIFY_I_CAL_A;
    cfg.lockin_ms = 800.0f;
    cfg.dir_ms = 400.0f;
    cfg.dir_step_rad = FOC_PI_F / 3.0f;
    cfg.sweep_steps = 1080U;               /* 3 电周期 × 360 步 */
    cfg.sweep_turns = 3.0f;                /* 多圈：平均局部传动误差（编码器/齿轮偏心） */
    cfg.sweep_step_ms = 8.0f;
    cfg.sweep_settle_ms = 200.0f;
    cfg.hyst_max_rad = 0.5f;               /* 正/反向零点差 > 28.6° 电角 → 回差/打滑 */
    cfg.quality_min = APP_IDENTIFY_QUALITY_MIN;
    cfg.ratio_tol = 0.2f;
    cfg.timeout_ms = 40000.0f;

    id_encoder_ctor(&s_id_encoder);
    if (s_id_encoder.init(&s_id_encoder, &cfg) != 0) {
        return -1;
    }
    if (app_foc_calib_begin() != 0) {
        return -1;                         /* FOC 须已使能（READY/RUN）且非 vtest */
    }
    s_id_encoder.reset(&s_id_encoder);

    s_identify_result = (app_motor_identify_result_t){0};
    s_identify_result_applied = false;
    s_identify_result.active = true;
    s_identify_sec_per_cycle = 1.0f / (float)cpu_hz;
    s_identify_last_cycle = intf_clock_get_cycle();
    s_identify_ticks = 0U;
    s_identify_oc_ticks = 0U;
    s_identify_enc_err_base = app_encoder_get_error_count(APP_ENCODER_ROTOR);
    s_identify_enc_jump_base = app_encoder_get_rotor_jump_count();
    s_identify_rotor_seq_valid = false;
    s_identify_state = APP_IDENTIFY_STATE_RUN;
    return 0;
}

void app_motor_identify_abort(void) {
    if (s_identify_state == APP_IDENTIFY_STATE_IDLE) {
        return;
    }
    identify_fail(APP_IDENTIFY_REASON_ABORTED);
}

void app_motor_identify_run_once(void) {
    app_foc_current_snapshot_t snap;
    float dt_s;

    if (s_identify_state == APP_IDENTIFY_STATE_IDLE) {
        return;
    }
    dt_s = identify_measure_dt();
    s_identify_ticks++;
    s_identify_result.elapsed_ms = s_identify_ticks;

    app_foc_get_snapshot(&snap);
    if (!identify_guard(&snap)) {
        return;
    }
    if (s_identify_state == APP_IDENTIFY_STATE_RUN) {
        identify_step_run(&snap, dt_s);
    } else {
        identify_step_verify(&snap);
    }
}

int app_motor_identify_apply(void) {
    app_motor_params_t* motor;

    if ((s_identify_state != APP_IDENTIFY_STATE_IDLE) || !s_identify_result.done) {
        return -1;
    }
    /* 先更新 FOC 运行零点（被拒则不写参数），再写电机参数（app_foc_enable 从参数重建） */
    if (app_foc_apply_encoder_offset(s_identify_result.offset_rad, s_identify_result.direction)
        != 0) {
        return -1;
    }
    motor = app_motor_params_mutable();
    motor->encoder.electrical_offset_rad = s_identify_result.offset_rad;
    motor->encoder.direction = s_identify_result.direction;
    s_identify_result_applied = true;
    return 0;
}

int app_motor_identify_save(void) {
    const app_motor_params_t* motor = app_motor_params_current();
    app_identify_record_t record;

    if (app_foc_get_state() != APP_FOC_STATE_OFF) {
        return -1;                        /* flash 操作仅停机窗口 */
    }
    if (s_identify_result.done && !s_identify_result_applied) {
        return APP_IDENTIFY_SAVE_PENDING; /* 新结果未 apply：拒绝把旧参数当结果落盘 */
    }
    if ((motor->pole_pairs == 0U) || !foc_finite(motor->encoder.electrical_offset_rad)
        || ((motor->encoder.direction != 1.0f) && (motor->encoder.direction != -1.0f))) {
        return -1;
    }
    memset(&record, 0, sizeof(record));
    record.version = APP_IDENTIFY_RECORD_VERSION;
    record.pole_pairs = motor->pole_pairs;
    record.offset_rad = motor->encoder.electrical_offset_rad;
    record.direction = motor->encoder.direction;
    return (app_param_store(APP_PARAM_KEY_FOC_ENCODER, &record, sizeof(record)) == 0) ? 0 : -1;
}

int app_motor_identify_restore(void) {
    app_motor_params_t* motor = app_motor_params_mutable();
    app_identify_record_t record;

    if (!app_param_is_ready()) {
        return 1;
    }
    if (app_param_load(APP_PARAM_KEY_FOC_ENCODER, &record, sizeof(record)) != 0) {
        return 1;                         /* 无记录：保持 YAML 默认值 */
    }
    if ((record.version != APP_IDENTIFY_RECORD_VERSION) || (record.pole_pairs != motor->pole_pairs)
        || !foc_finite(record.offset_rad)
        || ((record.direction != 1.0f) && (record.direction != -1.0f))) {
        return -1;                        /* 记录与当前电机不符：拒绝（需重新标定） */
    }
    motor->encoder.electrical_offset_rad = foc_wrap_2pi(record.offset_rad);
    motor->encoder.direction = record.direction;
    return 0;
}
