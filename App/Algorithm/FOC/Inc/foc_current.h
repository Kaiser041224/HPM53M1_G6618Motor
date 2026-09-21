/**
 * @file    foc_current.h
 * @brief   d/q 电流调节器（PI + 圆形电压限幅 + 抗饱和 + 前馈结构）
 * @author  Kaiser
 *
 * 设计（spec §3.4）：
 *   1) 给定圆形限幅（保角）→ 2) PI → 3) 前馈（可选）→ 4) 圆形电压限幅（保角）
 *   → 5) 抗饱和（未饱和累加 / 饱和衰减 + 积分单独限幅）
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef FOC_CURRENT_H
#define FOC_CURRENT_H

#include "foc_math.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 调节器配置
 */
typedef struct {
    float kp;              /**< 比例增益 [V/A] */
    float ki;              /**< 积分增益 [V/(A·s)] */
    float sample_time_s;   /**< 调用周期 [s] */
    uint8_t decoupling_en; /**< 解耦前馈开关（0/1） */
    float l_d;             /**< d 轴电感 [H]（前馈模型） */
    float l_q;             /**< q 轴电感 [H]（前馈模型） */
    float lambda;          /**< 永磁磁链 [Wb]（前馈模型） */
    float aw_decay;        /**< 饱和时积分衰减系数（0,1]；1.0 = 冻结 */
} foc_current_cfg_t;

/**
 * @brief 单步输入
 */
typedef struct {
    float i_d_ref, i_q_ref; /**< d/q 给定 [A] */
    float i_d_a, i_q_a;     /**< d/q 反馈 [A] */
    float v_bus_v;          /**< 母线电压 [V]（诊断用） */
    float v_max;            /**< 电压矢量限幅 [V] */
    float i_max;            /**< 电流矢量限幅 [A] */
    float omega_e_rad_s;    /**< 电角速度 [rad/s]（前馈用） */
} foc_current_in_t;

/**
 * @brief 单步输出
 */
typedef struct {
    float v_d, v_q;                 /**< 限幅后 d/q 电压 [V] */
    float i_d_ref_lim, i_q_ref_lim; /**< 限幅后 d/q 给定 [A] */
    bool saturated;                 /**< 本拍电压饱和标志 */
} foc_current_out_t;

typedef struct foc_current foc_current_t;

/**
 * @brief 初始化
 * @return 0 = 成功；-1 = 参数非法
 */
typedef int (*foc_current_init_fn)(foc_current_t* self, const foc_current_cfg_t* cfg);
/**
 * @brief 单步
 * @return 0 = 成功；-1 = 未初始化/参数空
 * @note 非有限输入（给定/反馈/ωe/限幅值）按 0 处理；输出保证有限
 */
typedef int (*foc_current_step_fn)(foc_current_t* self, const foc_current_in_t* in,
                                   foc_current_out_t* out);
/**
 * @brief 复位（清 d/q 积分器）
 */
typedef void (*foc_current_reset_fn)(foc_current_t* self);
/**
 * @brief 运行中更新增益
 */
typedef void (*foc_current_set_gains_fn)(foc_current_t* self, float kp, float ki);
/**
 * @brief 运行中开关解耦前馈
 * @param enable 0 = 关闭；非 0 = 开启
 */
typedef void (*foc_current_set_decoupling_fn)(foc_current_t* self, uint8_t enable);

/**
 * @brief 电流调节器对象
 */
struct foc_current {
    struct {
        foc_current_init_fn init;           /**< 初始化 */
        foc_current_step_fn step;           /**< 单步 */
        foc_current_reset_fn reset;         /**< 复位 */
        foc_current_set_gains_fn set_gains;             /**< 更新增益 */
        foc_current_set_decoupling_fn set_decoupling;   /**< 开关解耦前馈 */
    };

    float _kp, _ki;    /**< 增益 */
    float _ts;         /**< 采样周期 [s] */
    float _kits;       /**< ki × 采样周期 [V/A]（预计算，热路径） */
    uint8_t _decouple; /**< 前馈开关 */
    float _ld, _lq;    /**< 电感 [H] */
    float _lambda;     /**< 磁链 [Wb] */
    float _aw_decay;   /**< 积分衰减系数 */
    float _integ_d;    /**< d 轴积分项 [V] */
    float _integ_q;    /**< q 轴积分项 [V] */
    bool _inited;      /**< 初始化标志 */
};

/**
 * @brief 构造对象（绑定方法并清零状态）
 */
void foc_current_ctor(foc_current_t* self);

#ifdef __cplusplus
}
#endif

#endif /* FOC_CURRENT_H */
