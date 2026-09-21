/**
 * @file    algo_ramp.h
 * @brief   斜坡发生器（线性 / 指数 / 梯形 / 平滑阶跃）
 * @author  Kaiser
 *
 * Ramp Generator — Linear / Exponential / Trapezoidal / Smoothstep
 *
 * Linear mode:
 *   rate      = constant slope (output-units / s)
 *   per-step  = rate × step_time_s
 *
 * Exponential mode:
 *   rate      = time-constant τ (seconds)
 *   alpha     = 1 − exp(−step_time_s / τ)
 *   per-step  = current + alpha × (target − current)
 *
 * Trapezoidal mode (velocity + acceleration limits):
 *   rate      = max velocity v_max (output-units / s)
 *   accel_max = max acceleration a_max (output-units / s²)
 *   Phases:  ACCEL → CRUISE → DECEL → DONE
 *
 * Smoothstep mode (S-curve via duration):
 *   rate      = total duration (seconds), 0 = use rate-derived duration
 *   Perlin smoothstep:  s = 3t² − 2t³   where t ∈ [0, 1]
 *
 * step_time_s must match the fixed calling period.
 * ALGO_RAMP_EPSILON controls the snap-to-target threshold.
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ALGO_RAMP_H
#define ALGO_RAMP_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ALGO_RAMP_EPSILON
# define ALGO_RAMP_EPSILON 1.0e-6f /**< 到位判定阈值 */
#endif

/**
 * @brief 斜坡工作模式
 */
typedef enum {
    ALGO_RAMP_MODE_LINEAR = 0,      /**< 线性 */
    ALGO_RAMP_MODE_EXPONENTIAL = 1, /**< 指数 */
    ALGO_RAMP_MODE_TRAPEZOIDAL = 2, /**< 梯形（含加速度限制） */
    ALGO_RAMP_MODE_SMOOTHSTEP = 3,  /**< 平滑阶跃（S 曲线） */
} algo_ramp_mode_t;

typedef struct algo_ramp algo_ramp_t;

/**
 * @brief 斜坡发生器配置
 */
typedef struct {
    algo_ramp_mode_t mode; /**< 工作模式 */
    float rate;            /**< 速率 / 时间常数 / 最大速度（按模式解释） */
    float step_time_s;     /**< 调用周期 [s] */
    float target;          /**< 目标值 */
    float initial;         /**< 初始值 */
    float accel_max;       /**< 最大加速度（梯形模式） */
    float duration_s;      /**< 总时长 [s]（平滑阶跃模式，0 = 由 rate 推导） */
} algo_ramp_cfg_t;

/**
 * @brief 初始化
 */
typedef int (*algo_ramp_init_fn)(algo_ramp_t* self, const algo_ramp_cfg_t* cfg);
/**
 * @brief 单步计算
 */
typedef float (*algo_ramp_step_fn)(algo_ramp_t* self);
/**
 * @brief 复位
 */
typedef void (*algo_ramp_reset_fn)(algo_ramp_t* self);
/**
 * @brief 设置目标值
 */
typedef int (*algo_ramp_set_target_fn)(algo_ramp_t* self, float target);
/**
 * @brief 设置当前值
 */
typedef int (*algo_ramp_set_current_fn)(algo_ramp_t* self, float current);
/**
 * @brief 读取当前值
 */
typedef float (*algo_ramp_get_current_fn)(const algo_ramp_t* self);
/**
 * @brief 读取目标值
 */
typedef float (*algo_ramp_get_target_fn)(const algo_ramp_t* self);
/**
 * @brief 查询是否到位
 */
typedef bool (*algo_ramp_is_done_fn)(const algo_ramp_t* self);

/**
 * @brief 斜坡发生器对象
 */
struct algo_ramp {
    struct {
        algo_ramp_init_fn init;               /**< 初始化 */
        algo_ramp_step_fn step;               /**< 单步计算 */
        algo_ramp_reset_fn reset;             /**< 复位 */
        algo_ramp_set_target_fn set_target;   /**< 设置目标值 */
        algo_ramp_set_current_fn set_current; /**< 设置当前值 */
        algo_ramp_get_current_fn get_current; /**< 读取当前值 */
        algo_ramp_get_target_fn get_target;   /**< 读取目标值 */
        algo_ramp_is_done_fn is_done;         /**< 查询是否到位 */
    };

    algo_ramp_mode_t _mode;                   /**< 工作模式 */
    float _inc;                               /**< 线性模式每步增量 */
    float _alpha;                             /**< 指数模式滤波系数 */
    float _a_max;                             /**< 最大加速度 */
    float _v_max;                             /**< 最大速度 */
    float _ts_s;                              /**< 调用周期 [s] */
    float _target;                            /**< 目标值 */
    float _initial;                           /**< 初始值 */
    float _current;                           /**< 当前值 */
    float _velocity;                          /**< 当前速度（梯形模式） */
    float _elapsed;                           /**< 已用时长 [s] */
    float _duration;                          /**< 总时长 [s] */
    uint8_t _phase;                           /**< 梯形模式阶段 */
    bool _inited;                             /**< 初始化标志 */
};

/**
 * @brief 构造斜坡发生器对象（绑定方法并清零状态）
 */
void algo_ramp_ctor(algo_ramp_t* self);

/**
 * @brief IEEE-754 有限值判断（基于位操作，-ffast-math 下仍有效）
 * @param x 待判断值
 * @return true = 有限值
 */
static inline bool algo_ramp_finite(float x) {
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    return (bits & 0x7F800000u) != 0x7F800000u;
}

#ifdef __cplusplus
}
#endif

#endif /* ALGO_RAMP_H */
