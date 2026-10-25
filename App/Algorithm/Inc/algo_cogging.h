/**
 * @file    algo_cogging.h
 * @brief   齿槽/传动波动角表：分箱、F/R 合成、插值与质量统计（纯算法）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ALGO_COGGING_H
#define ALGO_COGGING_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ALGO_ATTR_RAMFUNC
# if defined(__GNUC__)
#  define ALGO_ATTR_RAMFUNC __attribute__((section(".fast")))
# else
#  define ALGO_ATTR_RAMFUNC
# endif
#endif

#define ALGO_COGGING_ROTOR_POINTS  (512U)
#define ALGO_COGGING_OUTPUT_POINTS (256U)

/** @brief 单个角度分箱的正/反向 q 电流累计 */
typedef struct {
    float sum_fwd_a;  /**< 正向采样 q 电流和 [A] */
    float sum_rev_a;  /**< 反向采样 q 电流和 [A]（反向电流为负） */
    uint16_t count_fwd; /**< 正向样本数（单箱标定期远小于 65535） */
    uint16_t count_rev; /**< 反向样本数 */
} algo_cogging_bin_t;

/** @brief 表统计 */
typedef struct {
    float rms_nm;
    float peak_to_peak_nm;
    float mean_nm;
    uint16_t valid_bins;
    bool valid;
} algo_cogging_stats_t;

/** @brief 清空一个分箱 */
void algo_cogging_bin_reset(algo_cogging_bin_t* bin);
/** @brief 累计一次 q 电流采样；dir = +1 正向速度，-1 反向速度 */
void algo_cogging_bin_add(algo_cogging_bin_t* bin, float iq_a, int8_t dir);
/**
 * @brief 由正/反向 q 电流均值计算转矩
 * @param bin 分箱
 * @param kt_nm_per_a 电机 Kt [Nm/A]
 * @param reducer_ratio 减速比（输出侧表使用）
 * @param output_side true=输出侧转矩，false=电机侧转矩
 * @param torque_out 输出转矩 [Nm]
 * @return 0 = 正反方向样本均足够；-1 = 不足/非法
 */
int algo_cogging_bin_torque(const algo_cogging_bin_t* bin, float kt_nm_per_a,
                            float reducer_ratio, bool output_side, float* torque_out);

/**
 * @brief 环形角表双线性插值
 * @param data int16 表
 * @param points 点数（>1）
 * @param scale_nm 每 LSB 转矩 [Nm]
 * @param angle_rad 机械角 [rad]（自动回绕）
 * @return 插值转矩 [Nm]；参数非法返回 0
 */
ALGO_ATTR_RAMFUNC
float algo_cogging_interp_i16(const int16_t* data, uint16_t points, float scale_nm,
                              float angle_rad);

/**
 * @brief 原始转矩数组 → int16 角表，去直流并统计
 * @param torque_nm 各箱转矩；无效箱为 0（valid=false）
 * @param valid 各箱有效标记
 * @param points 点数
 * @param scale_nm int16 缩放 [Nm/LSB]
 * @param table_out 输出表
 * @param stats_out 统计（可为 NULL）
 * @return 0 = 成功；-1 = 参数/有效箱不足
 */
int algo_cogging_finalize(const float* torque_nm, const bool* valid, uint16_t points,
                          float scale_nm, int16_t* table_out, algo_cogging_stats_t* stats_out);

/**
 * @brief 直接从分箱生成角表；不要求调用方再分配 torque/valid 数组
 * @param bins 分箱数组
 * @param points 点数
 * @param kt_nm_per_a 电机 Kt
 * @param reducer_ratio 减速比
 * @param output_side true=输出侧表
 * @param scale_nm int16 缩放
 * @param table_out 输出表
 * @param stats_out 统计（可为 NULL）
 * @return 0 = 成功；-1 = 参数/有效箱不足
 */
int algo_cogging_finalize_bins(const algo_cogging_bin_t* bins, uint16_t points,
                               float kt_nm_per_a, float reducer_ratio, bool output_side,
                               float scale_nm, int16_t* table_out,
                               algo_cogging_stats_t* stats_out);

#ifdef __cplusplus
}
#endif

#endif /* ALGO_COGGING_H */
