/**
 * @file    foc_modulation.h
 * @brief   调制（min-max 零序注入，等价 SVM）— 纯数学，零硬件依赖
 * @author  Kaiser
 *
 * 流程：反 Clarke → 调制限幅（采样窗口）→ 零序注入 → 占空比 → 逐相钳位
 * 采样窗口约束：max(d)+min(d) = 1（注入性质），故 span ≤ 2·(duty_max−0.5)
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef FOC_MODULATION_H
#define FOC_MODULATION_H

#include "foc_math.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 调制配置
 */
typedef struct {
    float duty_max;  /**< 占空比上限（0.5, 1.0]；0.885 = 三电阻采样窗口约束；须有限） */
    float v_bus_min; /**< 最低母线电压 [V]（须有限且 > 0；低于则拒绝输出） */
} foc_modulation_cfg_t;

/**
 * @brief 单步调制：αβ 电压 → 三相占空比
 * @param cfg 配置
 * @param v_alpha α 轴电压 [V]
 * @param v_beta β 轴电压 [V]
 * @param v_bus_v 母线电压 [V]
 * @param duty_abc 输出三相占空比 [0,1]（返回 -1 时不修改）
 * @param v_scale_out 输出幅度缩放系数（1.0 = 未限幅；<1 = 幅度限幅触发；
 *                     不含逐相钳位；可为 NULL）
 * @return 0 = 成功；-1 = 输入/配置非法（调用方应输出零矢量）
 */
int foc_modulation_step(const foc_modulation_cfg_t* cfg, float v_alpha, float v_beta, float v_bus_v,
                        float duty_abc[3], float* v_scale_out);

/**
 * @brief 线性区相电压峰值上限（与 foc_modulation_step 的 span 限幅严格一致）
 *
 * 推导：min-max 注入下 span_max = (2·duty_max − 1)·v_bus；
 *       平衡正弦三相电压峰值 A 对应 span = √3·A；
 *       → A_max = (2·duty_max − 1)·v_bus / √3。
 * （旧实现用 /1.5 偏大 15.5%，会让 PI 误判未饱和、抗饱和失效。）
 *
 * @param duty_max 占空比上限（0.5, 1.0]
 * @param v_bus_v 母线电压 [V]
 * @return 相电压峰值上限 [V]；输入非有限返回 0
 */
float foc_modulation_vmax(float duty_max, float v_bus_v);

#ifdef __cplusplus
}
#endif

#endif /* FOC_MODULATION_H */
