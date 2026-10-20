/**
 * @file    algo_speed_observer.h
 * @brief   转子机械角二阶跟踪观测器（PLL，30kHz 热路径）
 * @author  Kaiser
 *
 * 输入：未加软件零点的转子机械角 θm [rad]（绝对单圈）。
 * 输出：机械角估计 θm_hat 与机械角速度 ωm [rad/s]。
 * FOC 侧换算：ωe = pole_pairs × direction × ωm；
 * 速度环侧换算：ω_out = ωm / reducer。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ALGO_SPEED_OBSERVER_H
#define ALGO_SPEED_OBSERVER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 观测器配置
 */
typedef struct {
    float bandwidth_rad_s; /**< 闭环带宽 [rad/s]（>0） */
    float sample_time_s;   /**< 标称采样周期 [s]（>0；实际 dt 由 step 传入） */
    float max_speed_rad_s; /**< 机械速度限幅 [rad/s]（>0） */
} algo_speed_observer_cfg_t;

typedef struct algo_speed_observer algo_speed_observer_t;

/** @brief 初始化；0 成功，-1 参数非法 */
typedef int (*algo_speed_observer_init_fn)(algo_speed_observer_t* self,
                                           const algo_speed_observer_cfg_t* cfg);
/** @brief 单步；返回机械速度 [rad/s]，omega_out 可为 NULL */
typedef float (*algo_speed_observer_step_fn)(algo_speed_observer_t* self, float theta_meas_rad,
                                             float dt_s, float* omega_out);
/** @brief 复位（首拍重新锁相） */
typedef void (*algo_speed_observer_reset_fn)(algo_speed_observer_t* self);
/** @brief 读取机械速度 [rad/s] */
typedef float (*algo_speed_observer_get_fn)(const algo_speed_observer_t* self);

/**
 * @brief 观测器对象
 */
struct algo_speed_observer {
    struct {
        algo_speed_observer_init_fn init;  /**< 初始化 */
        algo_speed_observer_step_fn step;  /**< 单步 */
        algo_speed_observer_reset_fn reset;/**< 复位 */
        algo_speed_observer_get_fn get_omega; /**< 读取机械速度 */
        algo_speed_observer_get_fn get_theta; /**< 读取机械角 */
    };

    float _kp;         /**< 相位比例增益 [1/s] */
    float _ki;         /**< 速度积分增益 [1/s²] */
    float _ts;         /**< 标称采样周期 [s] */
    float _max_speed;  /**< 机械速度限幅 [rad/s] */
    float _theta;      /**< 机械角估计 [rad] */
    float _omega;      /**< 机械速度估计 [rad/s] */
    bool _primed;      /**< 首拍已建立 */
    bool _inited;      /**< 初始化标志 */
};

/**
 * @brief 构造对象（绑定方法并清零状态）
 */
void algo_speed_observer_ctor(algo_speed_observer_t* self);

#ifdef __cplusplus
}
#endif

#endif /* ALGO_SPEED_OBSERVER_H */
