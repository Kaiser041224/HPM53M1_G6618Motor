/**
 * @file    app_inertia_identify.h
 * @brief   轴端总惯量/负载惯量辨识（双向转矩阶跃；隔离模块）
 * @author  Kaiser
 *
 * 流程：FOC READY/RUN、motion 空闲 → 每个点位分别做正转 F / 反转 R：
 *       零电流稳速 → 同向电流加速 → 驱动窗口 → 反向电流制动 → 制动窗口
 *       → 计算 J_total/J_load。每个点位 2 次，共 6 点 × 2 = 12 次。
 * 结果不自动生效；FOC OFF 后 `cal inertia apply` 写 RAM，再由 `save` 持久化。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_INERTIA_IDENTIFY_H
#define APP_INERTIA_IDENTIFY_H

#include "algo_inertia.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_INERTIA_IDENTIFY_MAX_POINTS (6U)
#define APP_INERTIA_IDENTIFY_MAX_RUNS   (2U * APP_INERTIA_IDENTIFY_MAX_POINTS)

/** @brief 辨识状态 */
typedef enum {
    APP_INERTIA_IDENTIFY_IDLE = 0,
    APP_INERTIA_IDENTIFY_SETTLE,
    APP_INERTIA_IDENTIFY_ACCEL,
    APP_INERTIA_IDENTIFY_MEASURE_POS,
    APP_INERTIA_IDENTIFY_NEG_SETTLE,
    APP_INERTIA_IDENTIFY_MEASURE_NEG,
    APP_INERTIA_IDENTIFY_DONE,
    APP_INERTIA_IDENTIFY_FAILED,
} app_inertia_identify_state_t;

/** @brief 失败原因 */
typedef enum {
    APP_INERTIA_IDENTIFY_FAIL_NONE = 0,
    APP_INERTIA_IDENTIFY_FAIL_STATE,
    APP_INERTIA_IDENTIFY_FAIL_FEEDBACK,
    APP_INERTIA_IDENTIFY_FAIL_TIMEOUT,
    APP_INERTIA_IDENTIFY_FAIL_SPEED,
    APP_INERTIA_IDENTIFY_FAIL_WINDOW,
    APP_INERTIA_IDENTIFY_FAIL_CALC,
    APP_INERTIA_IDENTIFY_FAIL_QUALITY,
    APP_INERTIA_IDENTIFY_FAIL_MOTION,
    APP_INERTIA_IDENTIFY_FAIL_ABORTED,
} app_inertia_identify_failure_t;

/** @brief 辨识快照 */
typedef struct {
    app_inertia_identify_state_t state;
    app_inertia_identify_failure_t failure;
    bool busy;                    /**< SETTLE/ACCEL/MEASURE 阶段 */
    bool result_valid;            /**< DONE 且计算/范围通过 */
    bool applied;                 /**< RAM 已 apply */
    algo_inertia_result_t result; /**< 总惯量/负载惯量 */
    float current_a;              /**< 激励 q 电流 [A] */
    float torque_nm;              /**< 轴端转矩幅值 [Nm] */
    float alpha_pos_rad_s2;       /**< 驱动窗口速度斜率（原始符号） */
    float alpha_neg_rad_s2;       /**< 制动窗口速度斜率（原始符号） */
    float speed_peak_rad_s;       /**< 过程最大 |轴端速度| */
    uint32_t elapsed_ms;
    float max_current_a;          /**< 本轮允许的最大激励电流 [A] */
    uint16_t run_index;           /**< 当前采样序号（点×方向，0起） */
    uint16_t run_count;           /**< 采样总数（点数×2方向） */
    uint16_t point_count;         /**< 点位总数 */
    uint16_t valid_runs;          /**< 有效采样数 */
    uint16_t valid_forward_runs;  /**< 有效正转采样数 */
    uint16_t valid_reverse_runs;  /**< 有效反转采样数 */
    float progress;               /**< 0~1 进度 */
    float median_j_total_kgm2;    /**< 中位总惯量 */
    float mad_j_total_kgm2;       /**< 中位绝对偏差 */
    float spread_ratio;           /**< (max-min)/median */
    float run_j_total[APP_INERTIA_IDENTIFY_MAX_RUNS]; /**< 每次采样总惯量 */
    float run_current[APP_INERTIA_IDENTIFY_MAX_RUNS]; /**< 每次采样电流 */
    float run_start_speed[APP_INERTIA_IDENTIFY_MAX_RUNS]; /**< 每次采样起始速度 */
    float run_stop_speed[APP_INERTIA_IDENTIFY_MAX_RUNS];  /**< 每次采样截断速度 */
    int8_t run_direction[APP_INERTIA_IDENTIFY_MAX_RUNS];   /**< +1 正转 / -1 反转 */
    uint16_t run_valid_mask;      /**< 位图：1=该次采样有效 */
} app_inertia_identify_snapshot_t;

/** @brief 初始化（boot 一次） */
void app_inertia_identify_init(void);
/**
 * @brief 启动辨识
 * @param max_current_a 本轮最大激励 q 电流 [A]（0=默认2.0；上限2A，并受 i_q_max 裁）
 * @note 每个点位执行正转/反转两次采样；共 6 点 × 2 = 12 次。
 * @return 0 = 启动；-1 = 参数/忙；-2 = FOC 非 READY/RUN；-3 = motion 活动；
 *         -4 = 其他辨识/vtest 活动；-5 = 无法取得 DIRECT 电流源
 */
int app_inertia_identify_start(float max_current_a);
/** @brief 中止（幂等）：清 q/d 给定，回 IDLE */
void app_inertia_identify_abort(void);
/** @brief 1kHz 任务入口 */
void app_inertia_identify_run_once(void);
/** @brief 是否在 SETTLE/ACCEL/MEASURE 中 */
bool app_inertia_identify_busy(void);
/** @brief 观测一致副本 */
void app_inertia_identify_get_snapshot(app_inertia_identify_snapshot_t* out);
/**
 * @brief OFF 时把结果中的负载惯量写入 motor.load_inertia_kgm2（RAM）
 * @return 0 = 成功；-1 = 无有效结果；-2 = FOC 非 OFF
 */
int app_inertia_identify_apply(void);
/** @brief 持久化当前 motor 参数（须已 apply 且 FOC OFF） */
int app_inertia_identify_save(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_INERTIA_IDENTIFY_H */
