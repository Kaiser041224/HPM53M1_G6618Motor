/**
 * @file    app_friction_identify.h
 * @brief   摩擦前馈自动辨识（速度闭环稳态采集 + 最小二乘拟合；隔离模块）
 * @author  Kaiser
 *
 * 流程（照编码器/电角校准风格）：start → 速度点序列（settle+sample）→ 拟合与门限校验
 * → DONE（结果待 apply/save）/ FAILED（不落盘）。辨识数据 = 稳态 motion 总力矩请求
 * （= 摩擦，与 FF/PI 分成无关）。结束自动 motion off。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_FRICTION_IDENTIFY_H
#define APP_FRICTION_IDENTIFY_H

#include "algo_friction.h"
#include "app_motion.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 辨识状态 */
typedef enum {
    APP_FRICTION_IDENTIFY_IDLE = 0,
    APP_FRICTION_IDENTIFY_SETTLING,
    APP_FRICTION_IDENTIFY_SAMPLING,
    APP_FRICTION_IDENTIFY_DONE,
    APP_FRICTION_IDENTIFY_FAILED,
} app_friction_identify_state_t;

/** @brief 失败原因（FAILED 时有效；失败不得 apply/save） */
typedef enum {
    APP_FRICTION_IDENTIFY_FAIL_NONE = 0,
    APP_FRICTION_IDENTIFY_FAIL_MOTION,      /**< 运动被撤销（10kHz 路径中止/保持超限） */
    APP_FRICTION_IDENTIFY_FAIL_COMMAND,     /**< 目标命令持续被拒（重试超限） */
    APP_FRICTION_IDENTIFY_FAIL_NOT_SETTLED, /**< 速度未达稳态（含单点超时） */
    APP_FRICTION_IDENTIFY_FAIL_FIT,         /**< 拟合拒绝（样本退化） */
    APP_FRICTION_IDENTIFY_FAIL_RESIDUAL,    /**< 拟合残差超限 */
    APP_FRICTION_IDENTIFY_FAIL_ASYMMETRY,   /**< 正反对称性超限 */
} app_friction_identify_failure_t;

/** @brief 辨识快照 */
typedef struct {
    app_friction_identify_state_t state;
    app_friction_identify_failure_t failure;
    algo_friction_params_t result;              /**< 拟合结果（coulomb/viscous 有效） */
    float residual_rms_nm;                      /**< 拟合残差 RMS [Nm] */
    float asymmetry_nm;                         /**< 正反对称偏差均值 [Nm] */
    uint32_t point_index;                       /**< 当前速度点 */
    float progress;                             /**< 进度 0~1 */
    uint32_t elapsed_ms;                        /**< 流程已用时 [ms] */
    uint32_t command_retries;                   /**< 目标命令累计重试 */
    app_motion_reason_t motion_reason;          /**< 撤销/拒绝时的 motion 原因 */
    uint32_t good_count, bad_count, hold_count; /**< 撤销时的反馈健康 */
    bool result_valid;                          /**< 结果有效（仅门限通过的 DONE 后 true） */
} app_friction_identify_snapshot_t;

/** @brief 初始化（boot 一次；复位状态） */
void app_friction_identify_init(void);
/**
 * @brief 启动辨识序列（自动运动；FOC READY/RUN + motion 已标定且未活动）
 * @param tolerance 门限：残差/对称偏差 ≤ tolerance×平均|力矩|；0=默认0.35，合法[0.05,1.0]
 * @return 0 = 开始；-1 = 门限非法/已在运行；-2 = 未标定/标定中；-3 = motion 活动中；
 *         -4 = FOC 非 READY/RUN
 */
int app_friction_identify_start(float tolerance);
/** @brief 中止：撤销活动并回 IDLE（幂等） */
void app_friction_identify_abort(void);
/** @brief 1kHz 任务入口：状态推进与采样（非辨识期立即返回） */
void app_friction_identify_run_once(void);
/** @brief 当前是否在辨识中（SETTLING/SAMPLING） */
bool app_friction_identify_busy(void);
/** @brief 观测一致副本 */
void app_friction_identify_get_snapshot(app_friction_identify_snapshot_t* out);
/**
 * @brief 应用结果到运行参数（经 app_motion_gains 写透；须 DONE 且 FOC OFF）
 * @return 0 = 成功；-1 = 结果无效/FOC 未 OFF
 */
int app_friction_identify_apply(void);
/** @brief 持久化当前参数（须 FOC OFF；通常 apply 后调用）；0成功 */
int app_friction_identify_save(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_FRICTION_IDENTIFY_H */
