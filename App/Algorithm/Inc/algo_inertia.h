/**
 * @file    algo_inertia.h
 * @brief   惯量辨识结果计算（纯算法，零硬件依赖）
 * @author  Kaiser
 *
 * 两段同速度方向的转矩阶跃：+T 与 -T。摩擦在速度方向不变时同号，
 * 用 α+ - α- 抵消恒定摩擦/负载转矩：
 *   J_total = 2·T / (α+ - α-)
 *   J_load  = max(0, J_total - J_rotor·reducer²)
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ALGO_INERTIA_H
#define ALGO_INERTIA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 辨识计算结果（轴端等效惯量） */
typedef struct {
    float j_total_kgm2;           /**< 轴端总惯量 [kg·m²] */
    float j_rotor_reflected_kgm2; /**< 转子惯量反射到轴端 [kg·m²] */
    float j_load_kgm2;            /**< 负载/减速器附加惯量 [kg·m²]（≥0） */
    bool clamped;                 /**< 测量总惯量小于转子反射，负载已钳0 */
} algo_inertia_result_t;

/**
 * @brief 多点 J_total 综合：中位数 + MAD/极差质量
 * @param j_total 各点测得的总轴端惯量数组
 * @param valid 各点有效标记
 * @param count 点数（1~16）
 * @param rotor_inertia_kgm2 转子侧惯量 [kg·m²]
 * @param reducer_ratio 减速比
 * @param out 中位数对应的总/负载结果
 * @param median_out 中位数 [kg·m²]（可 NULL）
 * @param mad_out 中位绝对偏差 [kg·m²]（可 NULL）
 * @param spread_out (max-min)/median（可 NULL）
 * @param valid_count_out 有效点数（可 NULL）
 * @return 0 = 成功；-1 = 参数/点数/全无效
 */
int algo_inertia_aggregate(const float* j_total_kgm2, const bool* valid, uint16_t count,
                           float rotor_inertia_kgm2, float reducer_ratio,
                           algo_inertia_result_t* out, float* median_out, float* mad_out,
                           float* spread_out, uint16_t* valid_count_out);

/**
 * @brief 由双向转矩阶跃计算总惯量与负载惯量
 * @param torque_nm 施加的轴端转矩幅值 [Nm]（>0）
 * @param alpha_pos_rad_s2 +T 窗口速度斜率 [rad/s²]
 * @param alpha_neg_rad_s2 -T 窗口速度斜率 [rad/s²]
 * @param rotor_inertia_kgm2 转子侧惯量 [kg·m²]（≥0）
 * @param reducer_ratio 减速比（>0）
 * @param out 输出结果
 * @return 0 = 成功；-1 = 非法/退化输入
 */
int algo_inertia_calculate(float torque_nm, float alpha_pos_rad_s2, float alpha_neg_rad_s2,
                           float rotor_inertia_kgm2, float reducer_ratio,
                           algo_inertia_result_t* out);

#ifdef __cplusplus
}
#endif

#endif /* ALGO_INERTIA_H */
