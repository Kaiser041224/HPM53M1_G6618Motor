/**
 * @file    foc_math.h
 * @brief   FOC 数学基础（Clarke/Park 变换、角度归一化、sincos）— 纯数学，零硬件依赖
 * @author  Kaiser
 *
 * 符号与坐标约定（唯一来源，见设计文档 §3.2）：
 *   - 相序 U → V → W；α 轴与 U 相轴重合
 *   - Clarke（幅值不变，2/3 系数）：iα = (2/3)(iu − iv/2 − iw/2)；iβ = (iv − iw)/√3
 *   - Park：id = iα·cosθ + iβ·sinθ；iq = −iα·sinθ + iβ·cosθ
 *   - 反 Park：vα = vd·cosθ − vq·sinθ；vβ = vd·sinθ + vq·cosθ
 *   - 反 Clarke：vu = vα；vv = −vα/2 + (√3/2)vβ；vw = −vα/2 − (√3/2)vβ
 *   - 电角度零点：θe = 0 ⇔ 转子 d 轴与 α（U 相）轴重合
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef FOC_MATH_H
#define FOC_MATH_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FOC_PI_F     3.14159265358979323846f /**< π */
#define FOC_TWO_PI_F (2.0f * FOC_PI_F)       /**< 2π */
#define FOC_SQRT3_F  1.73205080756887729353f /**< √3 */

/* 热路径段属性：映射到链接脚本 .fast（ILM）段（与 algo_pid 同约定） */
#ifndef ALGO_ENABLE_ILM
# define ALGO_ENABLE_ILM 1
#endif
#if ALGO_ENABLE_ILM
# define FOC_ATTR_RAMFUNC __attribute__((section(".fast")))
#else
# define FOC_ATTR_RAMFUNC
#endif

/**
 * @brief IEEE-754 有限值判断（位操作，-ffast-math 下仍有效）
 * @param x 待判断值
 * @return true = 有限值
 */
FOC_ATTR_RAMFUNC
static inline bool foc_finite(float x) {
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    return (bits & 0x7F800000u) != 0x7F800000u;
}

/**
 * @brief 角度归一化到 [0, 2π)（对任意幅值有效）
 * @param x 角度 [rad]
 * @return 归一化角度 [rad]
 */
FOC_ATTR_RAMFUNC
static inline float foc_wrap_2pi(float x) {
    float y = x * (1.0f / FOC_TWO_PI_F);
    y -= floorf(y);
    return y * FOC_TWO_PI_F;
}

/**
 * @brief 角度归一化到 (−π, π]
 * @param x 角度 [rad]
 * @return 归一化角度 [rad]
 */
FOC_ATTR_RAMFUNC
static inline float foc_wrap_pm_pi(float x) {
    float y = foc_wrap_2pi(x);
    return (y > FOC_PI_F) ? (y - FOC_TWO_PI_F) : y;
}

/**
 * @brief 同时求 sin/cos
 * @param theta 角度 [rad]
 * @param s 输出 sin
 * @param c 输出 cos
 */
FOC_ATTR_RAMFUNC
static inline void foc_sincos(float theta, float* s, float* c) {
    *s = sinf(theta);
    *c = cosf(theta);
}

/**
 * @brief Clarke 变换（幅值不变，2/3 系数）
 */
FOC_ATTR_RAMFUNC
static inline void foc_clarke(float iu, float iv, float iw, float* ia, float* ib) {
    *ia = (2.0f / 3.0f) * (iu - 0.5f * iv - 0.5f * iw);
    *ib = (iv - iw) / FOC_SQRT3_F;
}

/**
 * @brief Park 变换（预计算 sin/cos；热路径复用同一 θ 的 sincos）
 */
FOC_ATTR_RAMFUNC
static inline void foc_park_sc(float ia, float ib, float s, float c, float* id, float* iq) {
    *id = ia * c + ib * s;
    *iq = -ia * s + ib * c;
}

/**
 * @brief Park 变换（内部求 sin/cos）
 */
FOC_ATTR_RAMFUNC
static inline void foc_park(float ia, float ib, float theta, float* id, float* iq) {
    float s, c;
    foc_sincos(theta, &s, &c);
    foc_park_sc(ia, ib, s, c, id, iq);
}

/**
 * @brief 反 Park 变换（预计算 sin/cos）
 */
FOC_ATTR_RAMFUNC
static inline void foc_inv_park_sc(float vd, float vq, float s, float c, float* va, float* vb) {
    *va = vd * c - vq * s;
    *vb = vd * s + vq * c;
}

/**
 * @brief 反 Park 变换（内部求 sin/cos）
 */
FOC_ATTR_RAMFUNC
static inline void foc_inv_park(float vd, float vq, float theta, float* va, float* vb) {
    float s, c;
    foc_sincos(theta, &s, &c);
    foc_inv_park_sc(vd, vq, s, c, va, vb);
}

/**
 * @brief 反 Clarke 变换（相电压参考，三相和为零）
 */
FOC_ATTR_RAMFUNC
static inline void foc_inv_clarke(float va, float vb, float* vu, float* vv, float* vw) {
    *vu = va;
    *vv = -0.5f * va + (FOC_SQRT3_F * 0.5f) * vb;
    *vw = -0.5f * va - (FOC_SQRT3_F * 0.5f) * vb;
}

#ifdef __cplusplus
}
#endif

#endif /* FOC_MATH_H */
