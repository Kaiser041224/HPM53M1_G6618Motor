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

_Static_assert(sizeof(float) == 4, "foc_math requires IEEE-754 binary32 float");

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
 * @brief 角度归一化到 [0, 2π)（快速版）
 * @param x 角度 [rad]；要求有限（非有限返回 NaN，调用方须先经 foc_finite 过滤）
 * @return 归一化角度 [rad]
 * @note 有效精度范围 |x| ≲ 1e4 rad（float 精度所限；FOC 场景 θe 输入有界）。
 *       实测误差：≈1.7e-7 @ 小角度、≈3.4e-5 rad @ 1600 rad。
 */
FOC_ATTR_RAMFUNC
static inline float foc_wrap_2pi(float x) {
    float y = x * (1.0f / FOC_TWO_PI_F);
    y -= floorf(y);
    return y * FOC_TWO_PI_F;
}

/**
 * @brief 角度归一化到 (−π, π]
 * @param x 角度 [rad]；要求有限（同 foc_wrap_2pi）
 * @return 归一化角度 [rad]；+π 归入 +π 侧
 */
FOC_ATTR_RAMFUNC
static inline float foc_wrap_pm_pi(float x) {
    float y = foc_wrap_2pi(x);
    return (y > FOC_PI_F) ? (y - FOC_TWO_PI_F) : y;
}

/**
 * @brief 同时求 sin/cos
 * @param theta 角度 [rad]（调用方保证有界，如 [0, 2π)）
 * @param s 输出 sin
 * @param c 输出 cos
 * @note 实现为 sinf + cosf 两次 libm 调用（不依赖非标准 sincosf）；
 *       GCC 可能自动融合。25kHz 热路径单拍调用一次，实测开销可接受。
 */
FOC_ATTR_RAMFUNC
static inline void foc_sincos(float theta, float* s, float* c) {
    *s = sinf(theta);
    *c = cosf(theta);
}

/**
 * @brief Clarke 变换（幅值不变，2/3 系数）
 * @param iu U 相电流 [A]
 * @param iv V 相电流 [A]
 * @param iw W 相电流 [A]
 * @param ia 输出 iα [A]
 * @param ib 输出 iβ [A]
 * @note 期望 iu + iv + iw ≈ 0（三相无中线）
 */
FOC_ATTR_RAMFUNC
static inline void foc_clarke(float iu, float iv, float iw, float* ia, float* ib) {
    *ia = (2.0f / 3.0f) * (iu - 0.5f * iv - 0.5f * iw);
    *ib = (iv - iw) / FOC_SQRT3_F;
}

/**
 * @brief Park 变换（预计算 sin/cos；热路径复用同一 θ 的 sincos）
 * @param ia iα [A] @param ib iβ [A]
 * @param s cos 的同伴 sin(θe) @param c cos(θe)
 * @param id 输出 id [A] @param iq 输出 iq [A]
 * @note 参数顺序为 (s, c)，调用点勿交换
 */
FOC_ATTR_RAMFUNC
static inline void foc_park_sc(float ia, float ib, float s, float c, float* id, float* iq) {
    *id = ia * c + ib * s;
    *iq = -ia * s + ib * c;
}

/**
 * @brief Park 变换（内部求 sin/cos）
 * @param ia iα [A] @param ib iβ [A]
 * @param theta 电角度 [rad]
 * @param id 输出 id [A] @param iq 输出 iq [A]
 */
FOC_ATTR_RAMFUNC
static inline void foc_park(float ia, float ib, float theta, float* id, float* iq) {
    float s, c;
    foc_sincos(theta, &s, &c);
    foc_park_sc(ia, ib, s, c, id, iq);
}

/**
 * @brief 反 Park 变换（预计算 sin/cos）
 * @param vd d 轴电压 [V] @param vq q 轴电压 [V]
 * @param s sin(θe) @param c cos(θe)
 * @param va 输出 vα [V] @param vb 输出 vβ [V]
 */
FOC_ATTR_RAMFUNC
static inline void foc_inv_park_sc(float vd, float vq, float s, float c, float* va, float* vb) {
    *va = vd * c - vq * s;
    *vb = vd * s + vq * c;
}

/**
 * @brief 反 Park 变换（内部求 sin/cos）
 * @param vd d 轴电压 [V] @param vq q 轴电压 [V]
 * @param theta 电角度 [rad]
 * @param va 输出 vα [V] @param vb 输出 vβ [V]
 */
FOC_ATTR_RAMFUNC
static inline void foc_inv_park(float vd, float vq, float theta, float* va, float* vb) {
    float s, c;
    foc_sincos(theta, &s, &c);
    foc_inv_park_sc(vd, vq, s, c, va, vb);
}

/**
 * @brief 反 Clarke 变换（相电压参考，三相和为零）
 * @param va vα [V] @param vb vβ [V]
 * @param vu 输出 U 相电压参考 [V]
 * @param vv 输出 V 相电压参考 [V]
 * @param vw 输出 W 相电压参考 [V]
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
