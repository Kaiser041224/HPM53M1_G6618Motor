/**
 * @file    app_foc_current.h
 * @brief   FOC 电流环 30kHz 执行（读输入 → Algorithm/FOC → 写逆变桥）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_FOC_CURRENT_H
#define APP_FOC_CURRENT_H

#include <stdbool.h>
#include <stdint.h>
#include "id_rl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_FOC_IDENTIFY_SAMPLES (128U)
/** @brief 校准同步捕获；仅由FOC校准端口在临界区调用 */
int app_foc_current_identify_arm(float voltage, bool voltage_mode);
/** @brief 临时增益；仅由FOC校准端口在临界区调用 */
int app_foc_current_identify_pi(float kp, float ki);
/** @brief CALIB内部有限恒压租约 */
int app_foc_current_identify_voltage(float voltage, uint32_t cycles);
/** @brief CALIB内部临时强制 R*I 前馈开关（不修改用户配置；0=关闭，1=开启） */
int app_foc_current_identify_r_ff(uint8_t enable);
/** @brief 校准内部交替激励 */
int app_foc_current_identify_pulse(float amplitude, uint16_t half_period);
/** @brief 内部上限设置，调用方须保证OFF */
int app_foc_current_set_duty_ceiling(float ceiling);
/** @brief 设置反 Park 输出延迟补偿 [s]（0=关闭；software.yaml init 期消费，调试可覆盖） */
void app_foc_current_set_output_delay(float delay_s);
/** @brief 返回冻结缓冲，count达到128才允许任务读取 */
const id_rl_sample_t* app_foc_current_identify_data(uint16_t* count);

/**
 * @brief 电流环快照（Terminal / Ozone 观测）
 */
typedef struct {
    float theta_e_rad;            /**< 本拍电角度 [rad] */
    float omega_e_rad_s;          /**< 本拍电角速度 [rad/s] */
    float i_d_a, i_q_a;           /**< d/q 反馈 [A]（瞬时，单拍） */
    float speed_out_rad_s;        /**< 由 ωe 换算的轴端机械速度 [rad/s]（诊断/速度环） */
    bool speed_out_valid;         /**< speed_out 来自有效的 30kHz observer（非 foc_angle 回退） */
    float i_d_avg_a, i_q_avg_a;   /**< d/q 反馈慢平均 [A]（~32Hz 一阶低通，供终端观测） */
    float i_d_ref_a, i_q_ref_a;   /**< d/q 给定（限幅后）[A] */
    float i_q_cogging_a;          /**< 本拍齿槽/传动补偿注入 q 电流 [A] */
    float v_d_v, v_q_v;           /**< d/q 电压 [V] */
    float duty_u, duty_v, duty_w; /**< 三相占空比 */
    float v_bus_v;                /**< 母线电压 [V] */
    float v_scale;                /**< 调制缩放（1.0 = 未限幅；不含逐相钳位） */
    bool  saturated;              /**< PI 圆形电压限幅触发（不含调制限幅） */
    bool  tripped;                /**< 快速过流跳闸（需 app_foc_disable() 清除） */
    bool  valid;                  /**< 本拍数据可信（false = 保护路径：零矢量/输入无效） */
    float voltage_max_v, voltage_utilization; /**< 电压圆上限及瞬时利用率 */
    uint32_t saturated_count; /**< 当前init周期内PI饱和拍累计，可与run_count作差 */
    uint32_t run_count;           /**< 健康拍计数（valid=true 时递增） */
    uint32_t fault_count;         /**< 保护路径计数（零矢量/无效输入） */
} app_foc_current_snapshot_t;

/**
 * Ozone 观测快照（.noncacheable.bss：启动清零 + 调试器直读，不受 D-Cache 影响）。
 * 每拍更新；与 app_foc_current_get_snapshot() 同一数据源。
 */
extern app_foc_current_snapshot_t g_foc_current_snapshot;

/**
 * @brief 初始化（构造算法对象；不使能桥）
 *
 * 参数生效语义：
 *   - 每拍 live：kp/ki（set_gains）、decoupling_en（set_decoupling，兼容旧组合开关）、
 *     current_ff.r_en/wl_en/bemf_en（set_r_ff/set_wl_ff/set_bemf_ff）、duty_max、
 *     i_q_max_a（每拍经 app_*_params_current 读取）
 *   - FOC OFF 期消费：l_d/l_q、r_ohm，以及由 `Kt_peak/(1.5·pole_pairs)` 派生的
 *     lambda；`param set` 对应字段被标为 `foc_off`，下一次 `foc on` 通过
 *     app_foc_current_reload_model() 生效（不重置积分器/boost）。
 *   - sample_time_s（= 1/pwm_freq_hz）仍为 init 期固定。
 */
void app_foc_current_init(void);

/**
 * @brief 从当前 motor/software 参数重载 R/L/λ 模型与增益（不重置积分器/boost）
 * @return 0 = 成功；-1 = 电流环未就绪或模型参数非法（保留旧模型）
 * @note 用于 param 修改后的 apply；调用方须保证 FOC OFF/未输出。
 */
int app_foc_current_reload_model(void);

/**
 * @brief 复位算法状态（积分器）
 */
void app_foc_current_reset(void);

/**
 * @brief 电流环是否初始化就绪（参数非法时为 false）
 * @return true = 就绪
 */
bool app_foc_current_is_ready(void);

/**
 * @brief 更新 FOC 电流 boost 预算（1kHz 调用）
 * @param current_a 实测电流矢量幅值 [A_peak]
 * @param dt_s 本拍时间 [s]
 * @note 连续值/boost 预算由 app_foc_current_init() 从 software 参数加载；
 *       FOC 电流环与 motion 转矩上限都读取 app_foc_current_get_i_limit()。
 */
void app_foc_current_limit_update(float current_a, float dt_s);

/**
 * @brief 当前 FOC 动态电流上限 [A_peak]
 */
float app_foc_current_get_i_limit(void);

/**
 * @brief 30kHz 执行：读电流/母线 → 电流环 → 调制 → 写三相占空比。
 * @param theta_e_rad 电角度 [rad]
 * @param omega_e_rad_s 电角速度 [rad/s]
 * @param i_d_ref d 轴给定 [A]
 * @param i_q_ref q 轴给定 [A]
 * @param duty_abc_out 输出三相占空比（供诊断；已写入逆变桥；可为 NULL）
 * @param saturated_out 输出电压饱和标志（可为 NULL）
 * @return 0 = 成功；-1 = 输入无效（已输出零矢量）
 */
int app_foc_current_run(float theta_e_rad, float omega_e_rad_s, float i_d_ref, float i_q_ref,
                        float duty_abc_out[3], bool* saturated_out);

/**
 * @brief 电流环（显式输入版本：ADC 完成回调内使用，输入为本拍新鲜采样）
 * @return 0 = 成功；-1 = 失败（已输出保护零矢量）
 */
int app_foc_current_run_fresh(float theta_e_rad, float omega_e_rad_s, float i_d_ref, float i_q_ref,
                              float i_u_a, float i_v_a, float i_w_a, float v_bus_v,
                              float duty_abc_out[3], bool* saturated_out);

/**
 * @brief 读取最近一拍快照
 */
void app_foc_current_get_snapshot(app_foc_current_snapshot_t* out);

/**
 * @brief 上一拍调制缩放（1.0 = 未限幅；<1 = 电压饱和）
 * @return 缩放系数（0.0~1.0）
 */
float app_foc_current_get_v_scale(void);

/**
 * @brief 控制层实际生效占空比上限（min(user duty_max, runtime ceiling)，默认0.95）。
 * @return 有效占空比上限（>0.5）
 * @note 0.95已完成空载速度试验，采样精度仍须示波确认；
 *       对 PI 圆限幅 / 调制 / vtest 一致生效，不修改用户 config。
 */
float app_foc_current_duty_max(void);

/**
 * @brief 死区补偿折算占空比幅值（hardware.inverter.deadtime_comp_ns × pwm_freq；0 = 关闭）
 * @return 补偿占空比 [duty]（0 ~ 0.02）
 * @note init / foc on（reload_model）期折算；vtest 诊断路径不注入。
 */
float app_foc_current_dt_comp_duty(void);

/**
 * @brief 开环电压矢量诊断（vtest）：不经电流环，按给定电压/角度直接调制输出。
 *        用于核实电流采样符号/相序映射/标度与电机直流增益（I ≈ v/R）。
 * @param volts 相电压峰值 [V]（内部限幅 0~2V）
 * @param theta_e_rad 电角度 [rad]
 * @param duration_s 持续时间 [s]（内部限幅 0.05~5s）
 * @return 0 = 成功；-1 = 状态不允许/参数非法
 */
int app_foc_current_vtest_start(float volts, float theta_e_rad, float duration_s);

/**
 * @brief 停止 vtest（零矢量）
 */
void app_foc_current_vtest_stop(void);

/**
 * @brief vtest 是否进行中
 * @return true = 进行中
 */
bool app_foc_current_vtest_active(void);

/**
 * @brief vtest 单拍执行（由 app_foc 在 READY/RUN 状态调用）
 * @return 0 = 已执行；-1 = 已结束/失败
 */
int app_foc_current_vtest_step(void);

/**
 * @brief vtest 单拍执行（显式输入版本：ADC 完成中断内使用，本拍新鲜采样 + 真实 dt）
 * @param i_u_a/i_v_a/i_w_a 本拍三相电流 [A]
 * @param v_bus_v 母线电压 [V]
 * @param dt_s 本拍间隔 [s]（≤0/非有限时退回标称 pwm 周期）
 * @return 0 = 已执行；-1 = 已结束/失败（已零矢量）
 */
int app_foc_current_vtest_step_fresh(float i_u_a, float i_v_a, float i_w_a, float v_bus_v,
                                     float dt_s);

/** 波形捕获最大样本数（30kHz × 128 ≈ 4.3ms） */
#define APP_FOC_TRACE_MAX (128U)

/**
 * @brief 捕获样本（d/q 电流、电压、电角度）
 */
typedef struct {
    float i_d_a, i_q_a;   /**< d/q 电流 [A] */
    float v_d_v, v_q_v;   /**< d/q 电压 [V] */
    float theta_e_rad;    /**< 电角度 [rad] */
} app_foc_trace_sample_t;

/**
 * @brief 布置捕获（下一拍开始；采满 APP_FOC_TRACE_MAX 后自动停止）
 * @return 0 = 成功；-1 = 参数/状态错误
 */
int app_foc_trace_arm(void);

/**
 * @brief 读取捕获数据
 * @param count 输出已捕获样本数
 * @return 数据指针（长度 ≥ count）
 */
const app_foc_trace_sample_t* app_foc_trace_data(uint16_t* count);

/**
 * @brief 清空捕获缓冲
 */
void app_foc_trace_reset(void);

/**
 * @brief 快速过流是否已跳闸（|i_dq| 连续超限）
 * @return true = 已跳闸
 */
bool app_foc_current_is_tripped(void);

/**
 * @brief 输出零矢量（duty = 0.5/0.5/0.5），不改变算法状态
 * @note 命令式零矢量（使能/关闭/待机）：不改变 valid/fault_count
 */
void app_foc_current_zero_vector(void);

/**
 * @brief 保护式零矢量：零矢量 + 快照 valid=false + fault_count++
 * @note 供保护路径使用（电流环内部失败 / 换相数据停摆等）
 */
void app_foc_current_protect(void);

/**
 * @brief 保护原因枚举（g_foc_protect_counts 下标）
 */
typedef enum {
    APP_FOC_PROT_READ = 0,   /**< 模拟量读取失败 */
    APP_FOC_PROT_VBUS,       /**< 母线电压非法/过低 */
    APP_FOC_PROT_PI,         /**< 电流环步进失败 */
    APP_FOC_PROT_MOD,        /**< 调制失败 */
    APP_FOC_PROT_DUTY,       /**< 占空比下发失败 */
    APP_FOC_PROT_TRIP,       /**< 快速过流跳闸 */
    APP_FOC_PROT_ENC,        /**< 换相角停摆/无效（app_foc 侧） */
    APP_FOC_PROT_COUNT
} app_foc_prot_reason_t;

/** 保护路径分原因计数（.noncacheable.bss；Ozone/RTT 观测） */
extern volatile uint32_t g_foc_protect_counts[APP_FOC_PROT_COUNT];

/**
 * @brief 记录一次保护路径并注明原因（g_foc_protect_counts[reason]++）
 */
void app_foc_current_protect_reason(uint8_t reason);

#ifdef __cplusplus
}
#endif

#endif /* APP_FOC_CURRENT_H */
