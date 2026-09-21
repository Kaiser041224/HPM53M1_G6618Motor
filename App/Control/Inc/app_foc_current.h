/**
 * @file    app_foc_current.h
 * @brief   FOC 电流环 25kHz 执行（读输入 → Algorithm/FOC → 写逆变桥）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_FOC_CURRENT_H
#define APP_FOC_CURRENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 电流环快照（Terminal / Ozone 观测）
 */
typedef struct {
    float theta_e_rad;            /**< 本拍电角度 [rad] */
    float omega_e_rad_s;          /**< 本拍电角速度 [rad/s] */
    float i_d_a, i_q_a;           /**< d/q 反馈 [A]（瞬时，单拍） */
    float i_d_avg_a, i_q_avg_a;   /**< d/q 反馈慢平均 [A]（~32Hz 一阶低通，供终端观测） */
    float i_d_ref_a, i_q_ref_a;   /**< d/q 给定（限幅后）[A] */
    float v_d_v, v_q_v;           /**< d/q 电压 [V] */
    float duty_u, duty_v, duty_w; /**< 三相占空比 */
    float v_bus_v;                /**< 母线电压 [V] */
    float v_scale;                /**< 调制缩放（1.0 = 未限幅；不含逐相钳位） */
    bool  saturated;              /**< PI 圆形电压限幅触发（不含调制限幅） */
    bool  tripped;                /**< 快速过流跳闸（需 app_foc_disable() 清除） */
    bool  valid;                  /**< 本拍数据可信（false = 保护路径：零矢量/输入无效） */
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
 *   - 每拍 live：kp/ki（set_gains）、decoupling_en（set_decoupling）、
 *     duty_max、i_q_max_a（每拍经 app_*_params_current 读取）
 *   - init 期固定：sample_time_s（= 1/pwm_freq_hz）、l_d/l_q/lambda（motor 域手册值）
 *     修改后需重新调用 app_foc_current_init()
 */
void app_foc_current_init(void);

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
 * @brief 25kHz 执行：读电流/母线 → 电流环 → 调制 → 写三相占空比。
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
 * @brief 读取最近一拍快照
 */
void app_foc_current_get_snapshot(app_foc_current_snapshot_t* out);

/**
 * @brief 上一拍调制缩放（1.0 = 未限幅；<1 = 电压饱和）
 * @return 缩放系数（0.0~1.0）
 */
float app_foc_current_get_v_scale(void);

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

/** 波形捕获最大样本数（25kHz × 128 ≈ 5.1ms） */
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

#ifdef __cplusplus
}
#endif

#endif /* APP_FOC_CURRENT_H */
