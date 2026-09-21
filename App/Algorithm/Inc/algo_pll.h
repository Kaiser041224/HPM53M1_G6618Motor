/**
 * @file    algo_pll.h
 * @brief   软件锁相环（周期跟踪 / 相位锁定）
 * @author  Kaiser
 *
 * Software PLL — Period Tracker / Phase-Locked Loop
 *
 * Two modes:
 *
 *   PERIOD_TRACKER  (mode 0, backward compatible):
 *     T_measured → [PI] → _period → _freq = 1/_period
 *     pll.step(&pll, period) returns tracked frequency Hz.
 *
 *   PHASE_LOCKED    (mode 1, true PLL):
 *     phase_err → [PI loop filter] → ω → [NCO] → φ → sin/cos
 *     pll.step_error(&pll, err_rad) returns tracked frequency Hz.
 *
 * Units:
 *   freq  = Hz
 *   omega = rad/s
 *   phase = rad  ([0, 2π) or [-π, π) depending on context)
 *   phase_error = rad  (accepted [-π, π), internally re-wrapped)
 *
 * sample_time_s must match the fixed calling period.
 * For ISR use, prefer step_error() which avoids sinf/cosf/atan2f.
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ALGO_PLL_H
#define ALGO_PLL_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALGO_PLL_PI_F     3.14159265358979323846f /**< π */
#define ALGO_PLL_TWO_PI_F (2.0f * ALGO_PLL_PI_F)  /**< 2π */

#ifndef ALGO_PLL_ENABLE_SINCOS_OUTPUT
# define ALGO_PLL_ENABLE_SINCOS_OUTPUT 1
#endif

#ifndef ALGO_PLL_ENABLE_ATAN2_INPUT
# define ALGO_PLL_ENABLE_ATAN2_INPUT 1
#endif

/**
 * @brief 锁相环工作模式
 */
typedef enum {
    ALGO_PLL_MODE_PERIOD_TRACKER = 0, /**< 周期跟踪（兼容模式） */
    ALGO_PLL_MODE_PHASE_LOCKED = 1,   /**< 相位锁定（真 PLL） */
} algo_pll_mode_t;

typedef struct algo_pll algo_pll_t;

/**
 * @brief 锁相环配置
 */
typedef struct {
    float kp;               /**< 环路比例增益 */
    float ki;               /**< 环路积分增益 */
    float period_nominal;   /**< 标称周期 [s]（周期跟踪模式） */
    float period_min;       /**< 周期下限 [s] */
    float period_max;       /**< 周期上限 [s] */
    algo_pll_mode_t mode;   /**< 工作模式 */
    float sample_time_s;    /**< 采样周期 [s]（相位锁定模式） */
    float freq_nominal_hz;  /**< 标称频率 [Hz] */
    float freq_min_hz;      /**< 频率下限 [Hz] */
    float freq_max_hz;      /**< 频率上限 [Hz] */
    float phase_init_rad;   /**< 初始相位 [rad] */
    float phase_offset_rad; /**< 相位偏置 [rad] */
} algo_pll_cfg_t;

/**
 * @brief 初始化
 */
typedef int (*algo_pll_init_fn)(algo_pll_t* self, const algo_pll_cfg_t* cfg);
/**
 * @brief 周期跟踪单步（输入测量周期）
 */
typedef float (*algo_pll_step_fn)(algo_pll_t* self, float period);
/**
 * @brief 复位
 */
typedef void (*algo_pll_reset_fn)(algo_pll_t* self);
/**
 * @brief 读取跟踪频率
 */
typedef float (*algo_pll_get_freq_fn)(const algo_pll_t* self);
/**
 * @brief 读取跟踪周期
 */
typedef float (*algo_pll_get_period_fn)(const algo_pll_t* self);
/**
 * @brief 相位误差单步（ISR 友好，避免 sinf/cosf/atan2f）
 */
typedef float (*algo_pll_step_error_fn)(algo_pll_t* self, float phase_error_rad);
/**
 * @brief 相位测量单步
 */
typedef float (*algo_pll_step_phase_fn)(algo_pll_t* self, float phase_meas_rad);
/**
 * @brief 正余弦测量单步
 */
typedef float (*algo_pll_step_sincos_fn)(algo_pll_t* self, float sin_meas, float cos_meas);
/**
 * @brief 读取锁相相位
 */
typedef float (*algo_pll_get_phase_fn)(const algo_pll_t* self);
/**
 * @brief 读取角频率
 */
typedef float (*algo_pll_get_omega_fn)(const algo_pll_t* self);
/**
 * @brief 读取内部 sin
 */
typedef float (*algo_pll_get_sin_fn)(const algo_pll_t* self);
/**
 * @brief 读取内部 cos
 */
typedef float (*algo_pll_get_cos_fn)(const algo_pll_t* self);
/**
 * @brief 读取相位误差
 */
typedef float (*algo_pll_get_phase_err_fn)(const algo_pll_t* self);
/**
 * @brief 设置相位
 */
typedef int (*algo_pll_set_phase_fn)(algo_pll_t* self, float phase_rad);
/**
 * @brief 设置频率
 */
typedef int (*algo_pll_set_freq_fn)(algo_pll_t* self, float freq_hz);

/**
 * @brief 锁相环对象
 */
struct algo_pll {
    struct {
        algo_pll_init_fn init;                       /**< 初始化 */
        algo_pll_step_fn step;                       /**< 周期跟踪单步 */
        algo_pll_reset_fn reset;                     /**< 复位 */
        algo_pll_get_freq_fn get_freq;               /**< 读取频率 */
        algo_pll_get_period_fn get_period;           /**< 读取周期 */
        algo_pll_step_error_fn step_error;           /**< 相位误差单步 */
        algo_pll_step_phase_fn step_phase;           /**< 相位测量单步 */
        algo_pll_step_sincos_fn step_sincos;         /**< 正余弦测量单步 */
        algo_pll_get_phase_fn get_phase;             /**< 读取相位 */
        algo_pll_get_omega_fn get_omega;             /**< 读取角频率 */
        algo_pll_get_sin_fn get_sin;                 /**< 读取 sin */
        algo_pll_get_cos_fn get_cos;                 /**< 读取 cos */
        algo_pll_get_phase_err_fn get_phase_error;   /**< 读取相位误差 */
        algo_pll_set_phase_fn set_phase;             /**< 设置相位 */
        algo_pll_set_freq_fn set_freq;               /**< 设置频率 */
    };

    algo_pll_mode_t _mode;                           /**< 工作模式 */
    float _kp, _ki;                                  /**< 环路增益 */
    float _ts_s;                                     /**< 采样周期 [s] */
    float _period_nominal, _period_min, _period_max; /**< 周期范围 [s] */
    float _period;                                   /**< 当前周期 [s] */
    float _integral;                                 /**< 周期跟踪积分项 */
    float _freq;                                     /**< 当前频率 [Hz] */

    float _phase;                                    /**< 当前相位 [rad] */
    float _phase_offset;                             /**< 相位偏置 [rad] */
    float _phase_err;                                /**< 相位误差 [rad] */
    float _omega;                                    /**< 当前角频率 [rad/s] */
    float _omega_nominal, _omega_min, _omega_max;    /**< 角频率范围 [rad/s] */
    float _loop_integral;                            /**< 锁相环积分项 */

    float _phase_init;                               /**< 初始相位 [rad] */
    float _freq_nominal, _freq_min, _freq_max;       /**< 频率范围 [Hz] */

#if ALGO_PLL_ENABLE_SINCOS_OUTPUT
    float _sin, _cos;                                /**< 内部正余弦输出 */
#endif

    bool _inited;                                    /**< 初始化标志 */
};

/**
 * @brief 构造锁相环对象（绑定方法并清零状态）
 */
void algo_pll_ctor(algo_pll_t* self);

/**
 * @brief IEEE-754 有限值判断（基于位操作，-ffast-math 下仍有效）
 * @param x 待判断值
 * @return true = 有限值
 */
static inline bool algo_pll_finite(float x) {
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    return (bits & 0x7F800000u) != 0x7F800000u;
}

#ifdef __cplusplus
}
#endif

#endif /* ALGO_PLL_H */
