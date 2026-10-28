/**
 * @file    algo_rms.h
 * @brief   真有效值（滑动窗 True RMS / AC RMS）
 * @author  Kaiser
 *
 * RMS — Sliding-Window True RMS / AC RMS
 *
 * True RMS:  y = sqrt( mean(x²) )
 * AC RMS:    y = sqrt( mean(x²) − mean(x)² )
 *
 * O(1) running sum-of-squares per sample.
 *
 * MATLAB:
 *   true_rms = rms(x)
 *   ac_rms   = rms(x - mean(x))
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ALGO_RMS_H
#define ALGO_RMS_H

/* 热路径段属性（ILM / .fast）：保护链 RMS 累加与取值在 1kHz 保护节拍内调用，
 * 且经函数指针间接调用（无法内联）→ 必须 ILM 常驻，避免 XIP 取指风暴。
 * 非热路径（ctor/init/reset）不标注。 */
#ifndef ALGO_RMS_ATTR_RAMFUNC
#  define ALGO_RMS_ATTR_RAMFUNC __attribute__((section(".fast")))
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct algo_rms algo_rms_t;

/**
 * @brief 滑动窗 RMS 配置
 */
typedef struct {
    uint16_t window_size; /**< 窗口长度（采样点数） */
    float* buffer;        /**< 外部环形缓冲，长度 ≥ window_size */
    bool remove_dc;       /**< true = AC RMS（去除直流分量） */
} algo_rms_cfg_t;

/**
 * @brief 初始化
 */
typedef int (*algo_rms_init_fn)(algo_rms_t* self, const algo_rms_cfg_t* cfg);
/**
 * @brief 单步计算（更新窗口并返回 RMS）
 */
typedef float (*algo_rms_step_fn)(algo_rms_t* self, float x);
/**
 * @brief 复位
 */
typedef void (*algo_rms_reset_fn)(algo_rms_t* self);
/**
 * @brief 读取当前 RMS
 */
typedef float (*algo_rms_get_rms_fn)(const algo_rms_t* self);
/**
 * @brief 读取当前均方值
 */
typedef float (*algo_rms_get_mean_sq_fn)(const algo_rms_t* self);
/**
 * @brief 更新窗口并返回均方值（不取平方根）
 */
typedef float (*algo_rms_update_sq_fn)(algo_rms_t* self, float x);

/**
 * @brief 滑动窗 RMS 对象
 */
struct algo_rms {
    struct {
        algo_rms_init_fn init;               /**< 初始化 */
        algo_rms_step_fn step;               /**< 单步计算 */
        algo_rms_reset_fn reset;             /**< 复位 */
        algo_rms_get_rms_fn get_rms;         /**< 读取 RMS */
        algo_rms_get_mean_sq_fn get_mean_sq; /**< 读取均方值 */
        algo_rms_update_sq_fn update_sq;     /**< 更新并返回均方值 */
    };

    float* _buf;                             /**< 外部环形缓冲 */
    uint16_t _size;                          /**< 窗口长度 */
    uint16_t _count;                         /**< 已填充样本数 */
    float _inv_size;                         /**< 1 / 窗口长度 */
    uint16_t _idx;                           /**< 环形缓冲写入位置 */
    float _sum_sq;                           /**< 平方和 */
    float _sum;                              /**< 线性和 */
    float _y;                                /**< 上次 RMS 输出 */
    bool _remove_dc;                         /**< 去除直流分量 */
    bool _filled;                            /**< 窗口已填满 */
    bool _inited;                            /**< 初始化标志 */
};

/**
 * @brief 构造 RMS 对象（绑定方法并清零状态）
 */
void algo_rms_ctor(algo_rms_t* self);

/**
 * @brief IEEE-754 有限值判断（基于位操作，-ffast-math 下仍有效）
 * @param x 待判断值
 * @return true = 有限值
 */
/** @brief 有限性判定（1kHz 保护链；与 RMS 累加同段，避免跨段取指）
 *
 * 用 union 做 type punning 而非 memcpy：后者在 -Og 下不会被内联，
 * 会在 ILM 热路径里跳回 XIP 的 memcpy 取指（同 foc_finite 的写法）。
 */
ALGO_RMS_ATTR_RAMFUNC
static inline bool algo_rms_finite(float x) {
    union {
        float f;
        uint32_t u;
    } pun = {.f = x};

    return (pun.u & 0x7F800000u) != 0x7F800000u;
}

#ifdef __cplusplus
}
#endif

#endif /* ALGO_RMS_H */
