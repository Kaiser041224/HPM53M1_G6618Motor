/**
 * @file    algo_hyst.h
 * @brief   施密特触发器（滞回比较）
 * @author  Kaiser
 *
 * Schmitt Trigger — Hysteresis Comparator
 *
 * Boundary semantics (strict):
 *     x >  upper  →  state = true
 *     x <  lower  →  state = false
 *     x == upper  →  state unchanged
 *     x == lower  →  state unchanged
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ALGO_HYST_H
#define ALGO_HYST_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct algo_hyst algo_hyst_t;

/**
 * @brief 施密特触发器配置
 */
typedef struct {
    float upper;     /**< 上阈值（x > upper 置 true） */
    float lower;     /**< 下阈值（x < lower 置 false） */
    bool init_state; /**< 初始 / 复位状态 */
} algo_hyst_cfg_t;

/**
 * @brief 初始化
 */
typedef int (*algo_hyst_init_fn)(algo_hyst_t* self, const algo_hyst_cfg_t* cfg);
/**
 * @brief 单步比较
 */
typedef bool (*algo_hyst_step_fn)(algo_hyst_t* self, float x);
/**
 * @brief 复位到初始状态
 */
typedef void (*algo_hyst_reset_fn)(algo_hyst_t* self);
/**
 * @brief 读取当前状态
 */
typedef bool (*algo_hyst_get_state_fn)(const algo_hyst_t* self);
/**
 * @brief 强制设置当前状态
 */
typedef void (*algo_hyst_set_state_fn)(algo_hyst_t* self, bool state);
/**
 * @brief 更新上下阈值
 */
typedef int (*algo_hyst_set_thresholds_fn)(algo_hyst_t* self, float lower, float upper);

/**
 * @brief 施密特触发器对象
 */
struct algo_hyst {
    struct {
        algo_hyst_init_fn init;                     /**< 初始化 */
        algo_hyst_step_fn step;                     /**< 单步比较 */
        algo_hyst_reset_fn reset;                   /**< 复位 */
        algo_hyst_get_state_fn get_state;           /**< 读取状态 */
        algo_hyst_set_state_fn set_state;           /**< 设置状态 */
        algo_hyst_set_thresholds_fn set_thresholds; /**< 更新阈值 */
    };

    float _upper;                                   /**< 上阈值 */
    float _lower;                                   /**< 下阈值 */
    bool _state;                                    /**< 当前状态 */
    bool _init_state;                               /**< 初始状态 */
    bool _inited;                                   /**< 初始化标志 */
};

/**
 * @brief 构造施密特触发器对象（绑定方法并清零状态）
 */
void algo_hyst_ctor(algo_hyst_t* self);

/**
 * @brief IEEE-754 有限值判断（基于位操作，-ffast-math 下仍有效）
 * @param x 待判断值
 * @return true = 有限值
 */
static inline bool algo_hyst_finite(float x) {
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    return (bits & 0x7F800000u) != 0x7F800000u;
}

#ifdef __cplusplus
}
#endif

#endif /* ALGO_HYST_H */
