/**
 * @file    foc_angle.h
 * @brief   电角度链（机械角 → 电角度 + ωe 估计）— 纯数学，零硬件依赖
 * @author  Kaiser
 *
 * θe = wrap_2pi(p · dir · θm_raw − offset_rad)
 *   - θm_raw 为**未加软件零点**的编码器机械角 [0, 2π)
 *   - ωe 由 θe 差分 + 一阶低通（自动含方向符号）
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef FOC_ANGLE_H
#define FOC_ANGLE_H

#include "foc_math.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 电角度链配置
 */
typedef struct {
    uint8_t pole_pairs;  /**< 极对数（≥1） */
    float direction;     /**< 方向（+1.0 / −1.0） */
    float offset_rad;    /**< 电角度零点 [rad] */
    float speed_lpf_hz;  /**< ωe 低通截止 [Hz]（0 = 不滤波） */
    float sample_time_s; /**< 调用周期 [s] */
} foc_angle_cfg_t;

typedef struct foc_angle foc_angle_t;

/**
 * @brief 初始化
 * @return 0 = 成功；-1 = 参数非法
 */
typedef int (*foc_angle_init_fn)(foc_angle_t* self, const foc_angle_cfg_t* cfg);
/**
 * @brief 单步：机械角 → 电角度
 * @param self 对象
 * @param theta_m_raw_rad 机械角（未加软件零点）[rad]
 * @param omega_e_out 输出电角速度 [rad/s]（可为 NULL）
 * @return 电角度 [rad]
 */
typedef float (*foc_angle_step_fn)(foc_angle_t* self, float theta_m_raw_rad, float* omega_e_out);
/**
 * @brief 复位（清除差分历史与 ωe）
 */
typedef void (*foc_angle_reset_fn)(foc_angle_t* self);
/**
 * @brief 运行中更新零点/方向（辨识结果写入路径）
 */
typedef void (*foc_angle_set_offset_fn)(foc_angle_t* self, float offset_rad, float direction);

/**
 * @brief 电角度链对象
 */
struct foc_angle {
    struct {
        foc_angle_init_fn init;             /**< 初始化 */
        foc_angle_step_fn step;             /**< 单步 */
        foc_angle_reset_fn reset;           /**< 复位 */
        foc_angle_set_offset_fn set_offset; /**< 更新零点/方向 */
    };

    float _p;            /**< 极对数（float 化） */
    float _dir;          /**< 方向 */
    float _offset;       /**< 电角度零点 [rad] */
    float _alpha;        /**< ωe 低通系数 */
    float _ts;           /**< 采样周期 [s] */
    float _theta_e_prev; /**< 上拍电角度 [rad] */
    float _omega_e;      /**< 电角速度 [rad/s] */
    bool _primed;        /**< 差分已初始化 */
    bool _inited;        /**< 初始化标志 */
};

/**
 * @brief 构造对象（绑定方法并清零状态）
 */
void foc_angle_ctor(foc_angle_t* self);

#ifdef __cplusplus
}
#endif

#endif /* FOC_ANGLE_H */
