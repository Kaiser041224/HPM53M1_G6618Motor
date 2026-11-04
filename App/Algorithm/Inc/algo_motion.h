/** @file algo_motion.h
 * @brief 游标解算与速度PI（圈输入、轴端rad输出）
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef ALGO_MOTION_H
#define ALGO_MOTION_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    float integral;
    bool saturated; /**< 外部执行限幅（FOC限流/电压饱和/请求过期）；饱和时仅误差反向允许积分 */
} algo_motion_pi_t;
/** @brief 游标多圈解算；rotor/vernier在[0,1)，reducer/output_ratio 来自 motor.yaml
 * @param residual_out 可空；输出解算残差 |estimate−nearest| [0,0.25]（健康余量观测：
 *                    接近 0.25 表示 pair 一致性边际，提示相位漂移/装配变化）
 */
int algo_motion_vernier(float rotor, float vernier, float phase, float reducer_ratio,
                        float output_ratio, float* shaft_rad, float* residual_out);
/**
 * @brief 连续角（行程坐标）单拍累积：跨 0/2π 边界 unwrap
 *
 * 输入是单圈绝对角 [0,2π)（游标解算结果）。每拍按最短角差累积到连续角：
 * - `*valid == false`：重基准到 `raw_angle`（不继承旧圈数），`*rebased = true`
 * - `|delta| >= max_delta`：判跳变，本拍不累积且 `*valid = false`（下拍重基准）
 * - 其余：`*continuous += delta`
 *
 * @param raw_angle 本拍单圈角 [rad]
 * @param prev_raw 上拍单圈角 [rad]
 * @param max_delta 跳变阈值 [rad]（必须 > 0）
 * @param continuous 连续角（输入/输出）[rad]
 * @param valid 累积器有效标志（输入/输出）
 * @param rebased 输出：本拍是否发生重基准（可为 NULL）
 * @return 0 = 正常累积或重基准；-1 = 判跳变（valid 已置 false）；-2 = 参数非法
 */
int algo_motion_continuous_step(float raw_angle, float prev_raw, float max_delta,
                                float* continuous, bool* valid, bool* rebased);

/** @brief ODrive POS_FILTER 二阶临界阻尼滤波状态（pos/vel 设定，ZOH 输入间平滑） */
typedef struct {
    float pos_setpoint_rad;       /**< 滤波后位置设定 [rad] */
    float vel_setpoint_rad_s;     /**< 滤波后速度设定 [rad/s]（含 input_vel 跟踪） */
    float accel_setpoint_rad_s2;  /**< 本拍滤波加速度 [rad/s²]（惯量前馈用） */
} algo_motion_pos_filter_t;
/** @brief ODrive POS_FILTER 单步：bw钳位≤0.25/dt；bw≤0直通；无效输入不更新 */
void algo_motion_pos_filter_step(
    algo_motion_pos_filter_t* f, float input_pos, float input_vel, float bandwidth_rad_s, float dt);
/** @brief 位置P+速度前馈（单圈，误差取最短路径 ±π）
 *
 * 注意：**不能用于多圈/连续坐标**——400deg 对 40deg 的 360deg 误差会被折成 0。
 */
float algo_motion_position(float pos_setpoint, float pos_est, float pos_gain, float vel_ff);

/**
 * @brief 位置P+速度前馈（连续坐标，误差不 wrap）
 *
 * 用于多圈/增量模式：误差为 pos_setpoint − pos_est 的算术差，
 * 360deg 的差就是 360deg（不会被折成 0）。
 */
float algo_motion_position_cont(float pos_setpoint, float pos_est, float pos_gain, float vel_ff);
/** @brief 速度PI条件积分，输出轴端Nm，ki为连续增益 */
float algo_motion_speed(
    algo_motion_pi_t* pi, float target, float speed, float kp, float ki, float dt, float limit);
/**
 * @brief 目标线性斜坡（速度/力矩命令整形）
 * @param current 当前生效目标
 * @param target 新目标
 * @param rate 目标变化率（单位/秒；<=0 = 直通）
 * @param dt 本拍周期 [s]（<=0/非有限 = 保持当前值）
 * @return 本拍生效目标
 */
float algo_motion_ramp(float current, float target, float rate, float dt);
/**
 * @brief ODrive limitVel 语义的连续速度限幅（轴端转矩）
 *
 * 由速度误差余量生成允许转矩区间：
 *   Tmax = (vel_limit - speed) * gain
 *   Tmin = (-vel_limit - speed) * gain
 * 超速时只允许制动方向转矩，避免 bang-bang 清零造成转矩断续。
 *
 * @param torque 原始轴端转矩 [Nm]
 * @param speed 轴端速度 [rad/s]
 * @param vel_limit 速度上限 [rad/s]（<=0 = 关闭）
 * @param gain 限幅增益 [Nm/(rad/s)]（<=0 = 关闭）
 * @param limited 输出是否发生限幅（可为 NULL）
 * @return 限幅后轴端转矩 [Nm]
 */
float algo_motion_limit_vel(float torque, float speed, float vel_limit, float gain, bool* limited);
#ifdef __cplusplus
}
#endif
#endif
