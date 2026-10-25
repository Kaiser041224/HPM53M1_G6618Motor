/**
 * @file    app_cogging.h
 * @brief   齿槽/减速器周期波动补偿：角表、使能与 q 电流前馈
 * @author  Kaiser
 *
 * 两张表独立配置、独立使能：
 *   rotor  : 电机侧转矩 [Nm]，按转子机械角索引
 *   output : 减速器输出侧转矩 [Nm]，按输出轴机械角索引
 *
 * 注入点在 FOC 电流环 q 轴给定；模块初始化默认关闭，是否 boot 自动启用
 * 由 software.control.cogging.enable 决定。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_COGGING_H
#define APP_COGGING_H

#include "algo_cogging.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_COGGING_ROTOR_SCALE_NM  (0.0005f)
#define APP_COGGING_OUTPUT_SCALE_NM (0.001f)

/** @brief 两张角表及质量统计 */
typedef struct {
    int16_t rotor[ALGO_COGGING_ROTOR_POINTS];
    int16_t output[ALGO_COGGING_OUTPUT_POINTS];
    algo_cogging_stats_t rotor_stats;
    algo_cogging_stats_t output_stats;
    bool rotor_valid;
    bool output_valid;
} app_cogging_tables_t;

/** @brief 补偿运行快照 */
typedef struct {
    bool master_enabled;
    bool rotor_enabled;
    bool output_enabled;
    bool rotor_valid;
    bool output_valid;
    bool suspended;
    bool speed_limited;
    bool output_angle_valid;
    float gain;
    float output_angle_rad;
    algo_cogging_stats_t rotor_stats;
    algo_cogging_stats_t output_stats;
} app_cogging_snapshot_t;

/** @brief boot 初始化（不访问 flash；表由 app_cogging_store_load 恢复） */
void app_cogging_init(void);
/**
 * @brief 按 software.control.cogging 默认值应用总开关/表选择/增益
 * @note boot 在角表加载后调用；cal cogging apply 在表复制到 RAM 后调用。
 *       没有有效表时总开关保持关闭。
 */
void app_cogging_apply_defaults(void);
/** @brief 复制一组表；不自动使能 */
int app_cogging_set_tables(const app_cogging_tables_t* tables);
/** @brief 读取当前活动表（只读，供持久化） */
const app_cogging_tables_t* app_cogging_get_tables(void);
/** @brief 清空表并使能位失效 */
void app_cogging_clear_tables(void);

/** @brief 总开关；要求至少一张有效表 */
int app_cogging_set_master(bool enable);
/** @brief 转子表使能；要求表有效 */
int app_cogging_set_rotor_enable(bool enable);
/** @brief 输出角表使能；要求表有效 */
int app_cogging_set_output_enable(bool enable);
/** @brief 补偿增益 [0,2]，默认 1 */
int app_cogging_set_gain(float gain);

/** @brief 辨识/标定期间挂起（不改变使能位） */
void app_cogging_suspend(bool suspend);
/** @brief motion 轴端速度限幅状态；限幅时本拍不注入 */
void app_cogging_set_speed_limited(bool limited);
/** @brief motion 是否处于有效驱动状态；非 active 时不注入 */
void app_cogging_set_motion_active(bool active);
/** @brief motion 输出轴角快照（15kHz 更新） */
void app_cogging_set_output_angle(float angle_rad, bool valid);

/**
 * @brief FOC ISR 查询补偿 q 电流
 * @param rotor_rad 转子机械角 [rad]
 * @return 注入 q 电流 [A]；未使能/挂起/表无效时为 0
 */
float app_cogging_iq(float rotor_rad);

/** @brief 一致快照 */
void app_cogging_get_snapshot(app_cogging_snapshot_t* out);

#ifdef __cplusplus
}
#endif

#endif /* APP_COGGING_H */
