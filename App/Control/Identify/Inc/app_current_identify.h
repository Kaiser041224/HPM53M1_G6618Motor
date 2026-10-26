/**
 * @file app_current_identify.h
 * @brief 按需 Rs/Ld 辨识与电流PI自整定，结果显式应用
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef APP_CURRENT_IDENTIFY_H
#define APP_CURRENT_IDENTIFY_H
#include "id_rl.h"
/** @brief 正式辨识状态 */
typedef enum {
    APP_CURRENT_IDENTIFY_IDLE,
    APP_CURRENT_IDENTIFY_SETTLE,
    APP_CURRENT_IDENTIFY_RS,
    APP_CURRENT_IDENTIFY_LD_SETTLE,
    APP_CURRENT_IDENTIFY_LD,
    APP_CURRENT_IDENTIFY_PI_SETTLE,
    APP_CURRENT_IDENTIFY_PI,
    APP_CURRENT_IDENTIFY_ZERO_SETTLE,
    APP_CURRENT_IDENTIFY_ZERO_CAPTURE
} app_current_identify_state_t;
/** @brief 数据拒绝细分原因；不改变原reason语义 */
typedef enum {
    APP_CURRENT_IDENTIFY_DETAIL_NONE,
    APP_CURRENT_IDENTIFY_DETAIL_STATE,
    APP_CURRENT_IDENTIFY_DETAIL_TIMEOUT,
    APP_CURRENT_IDENTIFY_DETAIL_SNAPSHOT,
    APP_CURRENT_IDENTIFY_DETAIL_MOVEMENT,
    APP_CURRENT_IDENTIFY_DETAIL_CAPTURE_INVALID,
    APP_CURRENT_IDENTIFY_DETAIL_Q_CURRENT,
    APP_CURRENT_IDENTIFY_DETAIL_RIPPLE,
    APP_CURRENT_IDENTIFY_DETAIL_TRACKING,
    APP_CURRENT_IDENTIFY_DETAIL_PORT
} app_current_identify_detail_t;
/** @brief flash 中 FCUR 记录的只读快照（终端显示用） */
typedef struct {
    bool present; /**< flash 中存在 FCUR 记录 */
    bool valid;   /**< 记录与当前 PWM 频率/极对数匹配且参数合法 */
    float rs_ohm, ld_h, kp, ki, bandwidth_hz;
} app_current_identify_saved_t;

/** @brief 结果及诊断快照 */
typedef struct {
    bool active, done, failed;                  /**< done=模型整定完成；动态达标另看step.passed */
    app_current_identify_state_t state;
    float rs_ohm, ld_h, kp, ki, bandwidth_hz;
    float rs_residual_v, ld_residual_a, ld_repeat_error;
    float current_ref_a, progress;
    id_rl_step_result_t step;
    id_rl_step_result_t r_ff_step_off; /**< 同一 d 轴阶跃，R*I 关闭 */
    id_rl_step_result_t r_ff_step_on;  /**< 同一 d 轴阶跃，R*I 开启 */
    bool r_ff_tested;                  /**< 已完成 R*I off/on A/B */
    bool r_ff_recommended;             /**< R*I 开启未导致指标劣化 */
    uint32_t elapsed_ms;
    int reason;                                 /**< 0成功，1取消，2状态/数据，3拟合，4验收 */
    app_current_identify_state_t failure_stage; /**< 退出前阶段 */
    app_current_identify_detail_t detail;       /**< 数据拒绝具体原因 */
    uint16_t capture_count, capture_invalid_count; /**< 最后平台捕获统计 */
    uint8_t platform_index;                        /**< 0起算的平台序号 */
    float capture_mean_a, capture_voltage_v, capture_ripple_a, capture_q_peak_a;
    float capture_ref_a, theta_delta_rad;          /**< 平台给定及相对起点电角位移 */
    float capture_q_mean_a;                        /**< 长窗口q轴均值，区别于瞬时峰值 */
    float trial_bandwidth_hz, trial_kp, trial_ki;  /**< 最后尝试参数，不代表通过 */
    id_rl_step_result_t trial_step;                /**< 最后尝试指标，与已通过step分离 */
    uint8_t bandwidth_retries;                     /**< 因 step 未通过而自动降带宽的次数 */
    float ld_half4_h, ld_half8_h;                  /**< 两种交替脉冲半周期的独立结果 */
    int ld_fit_status; /**< 0成功，-1输入，-2窗口不足，-3激励不足，-4残差 */
    float ld_rejected_h, ld_rejected_residual_a; /**< 不可应用的失败诊断 */
    float pi_baseline_a, pi_baseline_rms_a; /**< 每次阶跃前基线均值及块内波动RMS */
    float zero_d_mean_a, zero_q_mean_a; /**< 同PWM运行状态下零电压基线，只诊断不扣除 */
    float zero_phase_a[3], driven_phase_a[3]; /**< 零电压及最后平台三相均值 */
    float excitation_theta_rad; /**< 实际固定激励角 */
    float rs_window_drift_a; /**< 后半窗口均值减前半窗口均值 */
    uint8_t ld_extra_batches; /**< 残差过大时追加64块次数，最多每种脉宽一次 */
    uint8_t capture_retries;  /**< PI 阶段捕获无效样本重试次数（瞬态保护拍恢复） */
    uint8_t age_runs;         /**< 保留结果的代龄：其后失败运行的次数（0=最新一轮） */
} app_current_identify_result_t;
/** @brief READY启动，要求编码器零点已应用；返回0或-1 */
int app_current_identify_start(void);
/** @brief 取消并恢复正式PI，不新增关机策略 */
void app_current_identify_abort(void);
/** @brief app_io的1kHz有界入口，空闲直接返回 */
void app_current_identify_run_once(void);
/** @brief 活动状态 */
bool app_current_identify_is_active(void);
/** @brief 任务上下文读取结果 */
void app_current_identify_get_result(app_current_identify_result_t* out);
/** @brief 最近一轮完整 PASS 结果的保留副本（新 run 失败不清空；重启清空）
 * @return true = 存在保留结果并已拷贝到 out */
bool app_current_identify_get_retained(app_current_identify_result_t* out);
/** @brief 最近一次 apply 是否回落使用了保留结果（终端提示用） */
bool app_current_identify_applied_retained(void);
/** @brief OFF时应用最近结果；0成功，-1非OFF，-2无结果，-3活动，-4硬件参数变更，
 *         -5动态step未通过（拒绝应用） */
int app_current_identify_apply(void);
/** @brief OFF时保存已应用结果 */
int app_current_identify_save(void);
/** @brief 读取 flash FCUR 记录供显示，不修改运行参数；0有记录、1无记录、-1不可读 */
int app_current_identify_get_saved(app_current_identify_saved_t* out);
/** @brief init前恢复匹配PWM频率/极对数的记录；0恢复、1无记录、-1不匹配 */
int app_current_identify_restore(void);
/** @brief OFF时清除 flash FCUR 记录；0成功（含本无记录），-1非OFF/活动/不可写 */
int app_current_identify_clear(void);
#endif
