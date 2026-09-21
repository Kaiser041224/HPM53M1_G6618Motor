/**
 * @file    algo_ffd.h
 * @brief   前馈补偿（线性 / 查表）
 * @author  Kaiser
 *
 * Feedforward — Linear / Lookup-Table
 *
 * Linear mode:   u = gain_sp × sp + gain_dv × dv + offset
 * Table mode:    u = interp1d(x_tbl, y_tbl, sp) + gain_dv × dv + offset
 *
 * MATLAB table generation:
 *   x_tbl = [10, 15, 20, 25, 30];           % breakpoints
 *   y_tbl = [0.48, 0.32, 0.24, 0.19, 0.16]; % measured feedforward
 *   → const float x[] = {10,15,20,25,30};
 *   → const float y[] = {0.48,0.32,0.24,0.19,0.16};
 *   → algo_ffd_cfg_t .x_tbl=x .y_tbl=y .n_pts=5
 *
 * Usage with PID:
 *   duty_ff = ffd.step(&ffd, v_in_meas, 0);
 *   duty_pid = pid.step(&pid, v_link_target, v_link_meas);
 *   duty = duty_pid + duty_ff;
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ALGO_FFD_H
#define ALGO_FFD_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "algo_pid.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 前馈工作模式
 */
typedef enum {
    ALGO_FFD_MODE_LINEAR = 0, /**< 线性：u = gain_sp·sp + gain_dv·dv + offset */
    ALGO_FFD_MODE_TABLE = 1,  /**< 查表：u = interp1d(sp) + gain_dv·dv + offset */
} algo_ffd_mode_t;

typedef struct algo_ffd algo_ffd_t;

/**
 * @brief 前馈配置
 */
typedef struct {
    algo_ffd_mode_t mode; /**< 工作模式 */
    float gain_sp;        /**< 设定值前馈增益 */
    float gain_dv;        /**< 扰动前馈增益 */
    float offset;         /**< 输出偏置 */
    const float* x_tbl;   /**< 查表断点数组（递增）；线性模式忽略 */
    const float* y_tbl;   /**< 查表输出数组；线性模式忽略 */
    uint16_t n_pts;       /**< 断点数量（≥2）；线性模式忽略 */
} algo_ffd_cfg_t;

/**
 * @brief 前馈初始化
 */
typedef int (*algo_ffd_init_fn)(algo_ffd_t* self, const algo_ffd_cfg_t* cfg);
/**
 * @brief 前馈单步计算
 */
typedef float (*algo_ffd_step_fn)(algo_ffd_t* self, float sp, float dv);
/**
 * @brief 前馈复位
 */
typedef void (*algo_ffd_reset_fn)(algo_ffd_t* self);

/**
 * @brief 前馈对象
 */
struct algo_ffd {
    struct {
        algo_ffd_init_fn init;   /**< 初始化 */
        algo_ffd_step_fn step;   /**< 单步计算 */
        algo_ffd_reset_fn reset; /**< 复位 */
    };

    algo_ffd_mode_t _mode;       /**< 当前工作模式 */
    float _gain_sp;              /**< 设定值前馈增益 */
    float _gain_dv;              /**< 扰动前馈增益 */
    float _offset;               /**< 输出偏置 */
    const float* _x_tbl;         /**< 查表断点数组 */
    const float* _y_tbl;         /**< 查表输出数组 */
    uint16_t _n_pts;             /**< 断点数量 */
    float _y;                    /**< 上次输出 */
    bool _inited;                /**< 初始化标志 */
};

/**
 * @brief 构造前馈对象（绑定方法并清零状态）
 */
void algo_ffd_ctor(algo_ffd_t* self);

/**
 * @brief IEEE-754 有限值判断（基于位操作，-ffast-math 下仍有效）
 * @param x 待判断值
 * @return true = 有限值
 */
static inline bool algo_ffd_finite(float x) {
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    return (bits & 0x7F800000u) != 0x7F800000u;
}

/* ════════════════════════════════════════════════════════════════════════
 *  PID + Feedforward  —  Unified Controller
 *
 *     u = pid(sp, pv) + ffd(sp, dv)
 *
 *  ctor / init / step / reset 一站式调用，内部持有 pid + ffd 两个对象。
 *  通过 get_pid() / get_ffd() 可访问各自方法（set_gains, set_target 等）。
 * ════════════════════════════════════════════════════════════════════════ */

typedef struct algo_pid_ffd algo_pid_ffd_t;

/**
 * @brief PID + 前馈联合控制器配置
 */
typedef struct {
    algo_ffd_cfg_t ffd; /**< 前馈配置 */
    algo_pid_cfg_t pid; /**< PID 配置 */
} algo_pid_ffd_cfg_t;

/**
 * @brief 联合控制器初始化
 */
typedef int (*algo_pid_ffd_init_fn)(algo_pid_ffd_t* self, const algo_pid_ffd_cfg_t* cfg);
/**
 * @brief 联合控制器单步计算
 */
typedef float (*algo_pid_ffd_step_fn)(algo_pid_ffd_t* self, float sp, float pv, float dv);
/**
 * @brief 联合控制器复位
 */
typedef void (*algo_pid_ffd_reset_fn)(algo_pid_ffd_t* self);

/**
 * @brief PID + 前馈联合控制器对象
 */
struct algo_pid_ffd {
    struct {
        algo_pid_ffd_init_fn init;   /**< 初始化 */
        algo_pid_ffd_step_fn step;   /**< 单步计算 */
        algo_pid_ffd_reset_fn reset; /**< 复位 */
    };

    algo_pid_t _pid;                 /**< 内部 PID 对象 */
    algo_ffd_t _ffd;                 /**< 内部前馈对象 */
    bool _inited;                    /**< 初始化标志 */
};

/**
 * @brief 构造联合控制器对象（绑定方法并清零状态）
 */
void algo_pid_ffd_ctor(algo_pid_ffd_t* self);

/**
 * @brief 获取内部 PID 对象
 * @param self 联合控制器对象
 * @return PID 对象指针；self 为 NULL 时返回 NULL
 */
algo_pid_t* algo_pid_ffd_get_pid(algo_pid_ffd_t* self);

/**
 * @brief 获取内部前馈对象
 * @param self 联合控制器对象
 * @return 前馈对象指针；self 为 NULL 时返回 NULL
 */
algo_ffd_t* algo_pid_ffd_get_ffd(algo_pid_ffd_t* self);

#ifdef __cplusplus
}
#endif

#endif /* ALGO_FFD_H */
