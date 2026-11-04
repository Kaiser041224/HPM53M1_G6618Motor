/**
 * @file    algo_mit.h
 * @brief   MIT 阻抗控制（PD + 前馈）纯算法
 * @author  Kaiser
 *
 * 语义：tau = Kp*(p_des - p) + Kd*(v_des - v) + tau_ff，无积分项。
 * 单位口径：位置 [rad]、速度 [rad/s]、Kp [Nm/rad]、Kd [Nm/(rad/s)]、前馈 [Nm]，均为轴端。
 *
 * 限幅链顺序（不可调换）：
 *   1. 入口参数与增益上限校验
 *   2. 增益斜坡（防主机突跳 Kp 造成冲击）
 *   3. 位置误差钳制（防满力矩冲击，但不拒绝输出 → 避免运行中突然失力）
 *   4. 前馈占比限幅
 *   5. 力矩上限（由调用方传入动态值，含 boost/DERATE）
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ALGO_MIT_H
#define ALGO_MIT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief MIT 控制配置（来自 software.yaml，调用方每次读取） */
typedef struct {
    float kp_max_nm_per_rad;   /**< Kp 上限 [Nm/rad] */
    float kd_max_nm_per_rad_s; /**< Kd 上限 [Nm/(rad/s)] */
    float gain_ramp_rate;      /**< 增益逼近速率 [(Nm/rad)/s]，<=0 = 直通 */
    float pos_err_max_rad;     /**< 位置误差钳制上限 [rad]，<=0 = 不限 */
    float t_ff_max_ratio;      /**< 前馈占力矩上限比例 (0,1]，<=0 = 关闭前馈 */
} algo_mit_cfg_t;

/** @brief MIT 主机请求（每拍） */
typedef struct {
    float p_des_rad;      /**< 目标位置（连续坐标）[rad] */
    float v_des_rad_s;    /**< 目标速度 [rad/s] */
    float kp_nm_per_rad;  /**< 刚度 [Nm/rad] */
    float kd_nm_per_rad_s;/**< 阻尼 [Nm/(rad/s)] */
    float t_ff_nm;        /**< 前馈力矩 [Nm] */
} algo_mit_cmd_t;

/** @brief MIT 运行状态（跨拍保持增益斜坡） */
typedef struct {
    float kp_eff_nm_per_rad;   /**< 斜坡后生效 Kp */
    float kd_eff_nm_per_rad_s; /**< 斜坡后生效 Kd */
    float pos_err_rad;         /**< 本拍位置误差（钳制后） */
    float torque_nm;           /**< 本拍输出力矩（限幅后） */
    bool saturated;            /**< 力矩被上限截断 */
    bool gain_limited;         /**< 请求增益超上限（已截断） */
    bool err_clamped;          /**< 位置误差被钳制 */
} algo_mit_state_t;

/** @brief 返回码 */
typedef enum {
    ALGO_MIT_OK = 0,          /**< 正常 */
    ALGO_MIT_BAD_PARAM = -1,  /**< 非有限值或负增益 */
    ALGO_MIT_GAIN_LIMIT = -2, /**< 请求增益超过上限（拒绝，不输出） */
} algo_mit_status_t;

/**
 * @brief 复位增益与状态（进入 MIT 模式时调用；增益从 0 软启动）
 * @param st 状态
 */
void algo_mit_reset(algo_mit_state_t* st);

/**
 * @brief 单拍 MIT 阻抗控制
 * @param cfg 配置（每拍可读）
 * @param st 状态（跨拍保持增益斜坡）
 * @param cmd 主机请求
 * @param pos_rad 当前位置（连续坐标）[rad]
 * @param vel_rad_s 当前速度 [rad/s]
 * @param torque_limit_nm 力矩上限（正数，含动态 boost/DERATE）；<=0 视为非法
 * @param dt 本拍周期 [s]
 * @return ALGO_MIT_OK 成功；ALGO_MIT_BAD_PARAM 参数非法；ALGO_MIT_GAIN_LIMIT 增益超上限
 * @note 出错时不更新 st->torque_nm 的语义由调用方决定（建议撤销输出）。
 */
int algo_mit_step(const algo_mit_cfg_t* cfg, algo_mit_state_t* st, const algo_mit_cmd_t* cmd,
                  float pos_rad, float vel_rad_s, float torque_limit_nm, float dt);

#ifdef __cplusplus
}
#endif

#endif /* ALGO_MIT_H */