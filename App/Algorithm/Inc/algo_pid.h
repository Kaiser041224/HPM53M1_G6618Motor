/**
 * @file    algo_pid.h
 * @brief   PID 控制器
 * @author  Kaiser
 *
 * PID Controller
 *
 * Pure algorithm library — no hardware / SDK dependencies.
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ALGO_PID_H
#define ALGO_PID_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── IEEE-754 finite check (memcpy-based, survives -ffast-math) ───────── */

/**
 * @brief IEEE-754 有限值判断（基于位操作，-ffast-math 下仍有效）
 * @param x 待判断值
 * @return true = 有限值
 */
static inline bool algo_pid_finite(float x) {
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    return (bits & 0x7F800000u) != 0x7F800000u;
}

/* ── Optional ILM deployment for hot control-loop paths ─────────────────
 * Set ALGO_ENABLE_ILM=0 (e.g. via build system) to keep algorithm code in
 * flash; default deploys hot paths to ILM (.fast) for deterministic 200kHz
 * inner-loop execution (AGENTS.md §1). */
#ifndef ALGO_ENABLE_ILM
# define ALGO_ENABLE_ILM 1
#endif
#if ALGO_ENABLE_ILM
# define ALGO_ATTR_RAMFUNC __attribute__((section(".fast")))
#else
# define ALGO_ATTR_RAMFUNC
#endif

/**
 * @brief PID 结构形式
 */
typedef enum {
    ALGO_PID_MODE_POSITIONAL = 0,  /**< 位置式 */
    ALGO_PID_MODE_INCREMENTAL = 1, /**< 增量式 */
} algo_pid_mode_t;

/**
 * @brief 抗积分饱和策略
 */
typedef enum {
    ALGO_PID_ANTIWINDUP_NONE = 0,     /**< 不处理 */
    ALGO_PID_ANTIWINDUP_CLAMP = 1,    /**< 钳位（积分反向回退） */
    ALGO_PID_ANTIWINDUP_BACKCALC = 2, /**< 反算（back-calculation） */
} algo_pid_antiwindup_t;

/**
 * @brief PID 配置
 */
typedef struct {
    algo_pid_mode_t mode;             /**< 结构形式（位置 / 增量） */
    float kp;                         /**< 比例增益 */
    float ki;                         /**< 积分增益 */
    float kd;                         /**< 微分增益 */
    float sample_time_s;              /**< 采样周期 [s] */
    float out_min;                    /**< 输出下限 */
    float out_max;                    /**< 输出上限 */
    float integral_min;               /**< 积分项下限 */
    float integral_max;               /**< 积分项上限 */
    algo_pid_antiwindup_t antiwindup; /**< 抗积分饱和策略 */
    float backcalc_coeff;             /**< 反算系数（BACKCALC 有效） */
    float deriv_filter_coeff;         /**< 微分一阶滤波系数 [0,1)（0 = 关闭） */
    bool deriv_on_measurement;        /**< 微分作用对象：true = 测量值，false = 误差 */
    float rate_limit;                 /**< 输出变化率上限 [1/s]（0 = 关闭） */
    float setpoint_weight_p;          /**< 比例设定值权重 [0,1] */
    float setpoint_weight_d;          /**< 微分设定值权重 [0,1] */
} algo_pid_cfg_t;

typedef struct algo_pid algo_pid_t;

/**
 * @brief 初始化
 */
typedef int (*algo_pid_init_fn)(algo_pid_t* self, const algo_pid_cfg_t* cfg);
/**
 * @brief 单步计算
 */
typedef float (*algo_pid_step_fn)(algo_pid_t* self, float setpoint, float measurement);
/**
 * @brief 复位（清除积分与历史状态）
 */
typedef void (*algo_pid_reset_fn)(algo_pid_t* self);
/**
 * @brief 在线更新增益
 */
typedef void (*algo_pid_set_gains_fn)(algo_pid_t* self, float kp, float ki, float kd);
/**
 * @brief 读取当前增益
 */
typedef void (*algo_pid_get_gains_fn)(const algo_pid_t* self, float* kp, float* ki, float* kd);

/**
 * @brief PID 控制器对象
 */
struct algo_pid {
    struct {
        algo_pid_init_fn init;                    /**< 初始化 */
        algo_pid_step_fn step;                    /**< 单步计算 */
        algo_pid_reset_fn reset;                  /**< 复位 */
        algo_pid_set_gains_fn set_gains;          /**< 更新增益 */
        algo_pid_get_gains_fn get_gains;          /**< 读取增益 */
    };

    float _kp, _ki, _kd;                          /**< 比例 / 积分 / 微分增益 */
    float _ki_T;                                  /**< ki × 采样周期 */
    float _kd_invT;                               /**< kd / 采样周期 */
    float _inv_T;                                 /**< 1 / 采样周期 */
    float _rate_limit_T;                          /**< 输出变化率上限 × 采样周期 */
    float _spw_p, _spw_d;                         /**< 比例 / 微分设定值权重 */
    float _out_min, _out_max;                     /**< 输出限幅 */
    float _integral_min, _integral_max;           /**< 积分限幅 */
    float _backcalc_coeff;                        /**< 反算系数 */
    float _deriv_fc;                              /**< 微分滤波系数 */
    bool _deriv_on_pv;                            /**< 微分作用于测量值 */

    float _integral;                              /**< 积分累加项 */
    float _prev_error;                            /**< 上次误差 */
    float _prev2_error;                           /**< 上上次误差 */
    float _prev_setpoint;                         /**< 上周设定值 */
    float _prev2_setpoint;                        /**< 上上周设定值 */
    float _prev_measurement;                      /**< 上周测量值 */
    float _prev2_measurement;                     /**< 上上周测量值 */
    float _prev_output;                           /**< 上次输出 */
    float _prev_dout;                             /**< 上次微分项输出 */
    float _deriv_state;                           /**< 微分子滤波状态 */
    bool _primed;                                 /**< 首次有效输入已初始化标志 */

    algo_pid_antiwindup_t _aw_strategy;           /**< 抗积分饱和策略 */
    bool _has_rate_limit;                         /**< 启用变化率限制 */
    bool _has_deriv_filter;                       /**< 启用微分滤波 */
    bool _initialized;                            /**< 初始化标志 */
};

extern const algo_pid_cfg_t ALGO_PID_CFG_DEFAULT; /**< 默认配置 */

/**
 * @brief 构造 PID 对象（绑定方法并清零状态）
 */
void algo_pid_ctor(algo_pid_t* self);

#ifdef __cplusplus
}
#endif

#endif /* ALGO_PID_H */
