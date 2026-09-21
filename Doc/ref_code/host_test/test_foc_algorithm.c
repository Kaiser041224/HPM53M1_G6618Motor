/**
 * @file    test_foc_algorithm.c
 * @brief   YHorizon-JM FOC 纯算法层离线验证（宿主机，无硬件）
 * @author  Kaiser
 *
 * 对本目录 ../reference_src 下的 Foc/ 算法层做离线验证：变换可逆性、限幅边界、
 * 角度 wrap 边界、查表三角函数精度、SVPWM 线性区、游标绝对位置解码、
 * 电流环量纲与母线电压增益调度。
 *
 * 设计意图：FOC 数学层是纯函数，可在宿主机上零成本回归。本工程的 25kHz 电流环
 * 同样应保持这一性质（见 docs/superpowers/specs/2026-09-21-foc-algorithm-design.md §11）。
 *
 * 编译：见本目录 build_and_run.ps1（MinGW gcc，无需 ARM 工具链）
 * 退出码：0 = 全部通过；1 = 有断言失败
 *
 * 本文件链接 ../reference_src/Foc/ 下的 .c 文件（GPLv3），故整体按 GPLv3 分发。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "config.h"
#include "foc_current.h"
#include "foc_math.h"
#include "foc_svpwm.h"
#include "foc_vernier.h"
#include "encoder.h"

#define TEST_TWO_PI     (6.283185307179586f)
#define TEST_CPR        ((float)ENCODER_CPR)
#define TEST_TEETH_1    ((float)CFG_VERNIER_TEETH1)
#define TEST_TEETH_2    ((float)CFG_VERNIER_TEETH2)

static int s_checks;
static int s_failures;

/**
 * @brief  断言辅助：记录并打印结果
 */
static void check(int condition, const char *name, const char *detail)
{
    s_checks++;
    if (condition != 0) {
        printf("  PASS  %-46s %s\n", name, detail);
    } else {
        s_failures++;
        printf("  FAIL  %-46s %s\n", name, detail);
    }
}

/* ------------------------------------------------------------------ */
/* 合成编码器码值（按上游 foc_vernier.c 自身假设的物理关系）            */
/* ------------------------------------------------------------------ */

static uint16_t code_from_fraction(float fraction)
{
    float wrapped = fraction - floorf(fraction);

    if (wrapped < 0.0f) {
        wrapped += 1.0f;
    }
    return (uint16_t)(wrapped * TEST_CPR);
}

/**
 * @brief  合成 30/31 游标编码器对：电机齿轮圈内分数 u 与旁置齿轮的 -N1/N2 圈
 * @param  total_turns  电机轴总圈数 T
 * @param  aux_dir      建模 config.h 的 CFG_ENC2_DIR
 */
static void synth_vernier(float total_turns, int aux_dir,
                          uint16_t *raw_motor, uint16_t *raw_aux)
{
    const float u = total_turns - floorf(total_turns);
    float aux_turns = -(TEST_TEETH_1 / TEST_TEETH_2) * total_turns;

    if (aux_dir < 0) {
        aux_turns = -aux_turns;
    }
    *raw_motor = code_from_fraction(u);
    *raw_aux = code_from_fraction(aux_turns);
}

/* ------------------------------------------------------------------ */
/* 1. 坐标变换                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief  用 libm 精确三角函数复现 Park / 反 Park，用于把"变换对本身"与"查表误差"分离
 */
static void exact_park(float alpha, float beta, float theta, float *d, float *q)
{
    const float c = cosf(theta);
    const float s = sinf(theta);

    *d = (alpha * c) + (beta * s);
    *q = (beta * c) - (alpha * s);
}

static void exact_inverse_park(float d, float q, float theta, float *alpha, float *beta)
{
    const float c = cosf(theta);
    const float s = sinf(theta);

    *alpha = (d * c) - (q * s);
    *beta = (d * s) + (q * c);
}

static void test_transforms(void)
{
    const int kN = 20000;
    int i;
    float worst_exact = 0.0f;   /* 精确三角函数下的往返误差 */
    float worst_lut = 0.0f;     /* 查表下的往返误差 */
    float worst_clarke = 0.0f;
    float worst_orth = 0.0f;    /* 查表正交性 |c²+s²−1| */

    printf("[1] 坐标变换：可逆性与三相等价性\n");

    for (i = 0; i < kN; i++) {
        float theta = (-2.0f * TEST_TWO_PI) +
                      (4.0f * TEST_TWO_PI) * (float)i / (float)kN;
        float ia = sinf(theta * 3.0f) * 4.0f;
        float ib = cosf(theta * 5.0f) * 3.0f;
        float ic = -(ia + ib); /* 三相无中线约束 */
        Foc_AlphaBeta_t ab;
        Foc_Dq_t dq;
        Foc_AlphaBeta_t back;
        float three_phase_alpha;
        float three_phase_beta;
        float exact_d;
        float exact_q;
        float exact_alpha;
        float exact_beta;
        float lut_sin;
        float lut_cos;
        float err;

        /* 两相 Clarke（上游） */
        ab = Foc_Clarke(ia, ib);

        /* 三相显式 Clarke（本工程 spec §3.2 约定）：应完全一致 */
        three_phase_alpha = (2.0f / 3.0f) * (ia - (ib * 0.5f) - (ic * 0.5f));
        three_phase_beta = (ib - ic) * FOC_INV_SQRT3;
        err = fabsf(ab.alpha - three_phase_alpha);
        if (err > worst_clarke) {
            worst_clarke = err;
        }
        err = fabsf(ab.beta - three_phase_beta);
        if (err > worst_clarke) {
            worst_clarke = err;
        }

        /* A. 变换对本身：用 libm 精确三角函数应严格可逆 */
        exact_park(ab.alpha, ab.beta, theta, &exact_d, &exact_q);
        exact_inverse_park(exact_d, exact_q, theta, &exact_alpha, &exact_beta);
        err = fabsf(exact_alpha - ab.alpha);
        if (err > worst_exact) {
            worst_exact = err;
        }
        err = fabsf(exact_beta - ab.beta);
        if (err > worst_exact) {
            worst_exact = err;
        }

        /* B. 查表版本：误差只应来自查表的正交性残差 c²+s² ≠ 1 */
        dq = Foc_Park(ab, theta);
        back = Foc_InversePark(dq, theta);
        err = fabsf(back.alpha - ab.alpha);
        if (err > worst_lut) {
            worst_lut = err;
        }
        err = fabsf(back.beta - ab.beta);
        if (err > worst_lut) {
            worst_lut = err;
        }
        Foc_SinCos(theta, &lut_sin, &lut_cos);
        err = fabsf(((lut_cos * lut_cos) + (lut_sin * lut_sin)) - 1.0f);
        if (err > worst_orth) {
            worst_orth = err;
        }
    }

    {
        char detail[112];

        snprintf(detail, sizeof(detail), "max |Δ| = %.3e", (double)worst_clarke);
        check(worst_clarke < 1e-4f, "Clarke 两相式 == 三相显式式", detail);

        snprintf(detail, sizeof(detail), "max |Δ| = %.3e（libm 三角函数）", (double)worst_exact);
        check(worst_exact < 1e-4f, "Park -> 反Park 严格可逆", detail);

        /* 往返误差 = 幅值 × |c²+s²−1|，是"缩放"误差而非旋转误差，对 FOC 无实际影响 */
        snprintf(detail, sizeof(detail), "max |c²+s²−1| = %.3e", (double)worst_orth);
        check(worst_orth < 3e-4f, "查表正交性残差（往返误差的唯一来源）", detail);

        snprintf(detail, sizeof(detail), "max |Δ| = %.3e = 幅值×正交性残差",
                 (double)worst_lut);
        check(worst_lut < 1e-3f, "查表往返误差（缩放误差，可忽略）", detail);
    }
}

/* ------------------------------------------------------------------ */
/* 2. DQ 圆形限幅                                                       */
/* ------------------------------------------------------------------ */

static void test_limit_dq(void)
{
    float d;
    float q;
    float mag;
    char detail[96];

    printf("[2] Foc_LimitDQ：边界与 d 轴优先级\n");

    d = 0.9f;
    q = 0.9f;
    Foc_LimitDQ(0.5f, &d, &q);
    mag = sqrtf((d * d) + (q * q));
    snprintf(detail, sizeof(detail), "d=%.4f q=%.4f |v|=%.4f", (double)d, (double)q, (double)mag);
    check((mag <= 0.5001f) && (d == 0.5f) && (q == 0.0f), "超限时钳到幅值上限且保 d 轴", detail);

    d = 0.49f;
    q = 0.9f;
    Foc_LimitDQ(0.5f, &d, &q);
    mag = sqrtf((d * d) + (q * q));
    snprintf(detail, sizeof(detail), "d=%.4f q=%.4f |v|=%.4f", (double)d, (double)q, (double)mag);
    check((fabsf(d - 0.49f) < 1e-6f) && (mag <= 0.5001f), "d 未超限时保持 d 不动", detail);

    d = 0.1f;
    q = 0.2f;
    Foc_LimitDQ(0.5f, &d, &q);
    snprintf(detail, sizeof(detail), "d=%.4f q=%.4f", (double)d, (double)q);
    check((fabsf(d - 0.1f) < 1e-6f) && (fabsf(q - 0.2f) < 1e-6f), "未超限时不改动", detail);
}

/* ------------------------------------------------------------------ */
/* 3. 角度 wrap 与方向镜像                                              */
/* ------------------------------------------------------------------ */

static void test_angle(void)
{
    int i;
    int bad_2pi = 0;
    int bad_pi = 0;
    int bad_mirror = 0;

    printf("[3] 角度 wrap 与编码器方向镜像\n");

    for (i = -100000; i <= 100000; i += 7) {
        float x = (float)i * 0.001f;
        float wrapped_2pi = Foc_WrapAngle0To2Pi(x);
        float wrapped_pi = Foc_WrapAngleToPi(x);

        if (!((wrapped_2pi >= 0.0f) && (wrapped_2pi < TEST_TWO_PI))) {
            bad_2pi++;
        }
        if (!((wrapped_pi >= -FOC_PI) && (wrapped_pi < FOC_PI))) {
            bad_pi++;
        }
    }

    /* 镜像关系：dir=-1 时 θ_mech + θ_aligned ≡ 2π */
    for (i = 1; i < 62832; i++) {
        float mech = (float)i * 0.0001f;
        float mirrored = Foc_ApplyEncoderDirToMechTheta(mech, -1);

        if (!((mirrored >= 0.0f) && (mirrored < TEST_TWO_PI))) {
            bad_mirror++;
        } else if (fabsf(Foc_WrapAngleToPi(mirrored + mech)) > 1e-4f) {
            bad_mirror++;
        }
    }

    {
        char detail[96];

        snprintf(detail, sizeof(detail), "越界 %d 次（±100 rad 扫描）", bad_2pi);
        check(bad_2pi == 0, "Foc_WrapAngle0To2Pi -> [0, 2π)", detail);
        snprintf(detail, sizeof(detail), "越界 %d 次", bad_pi);
        check(bad_pi == 0, "Foc_WrapAngleToPi -> (-π, π]", detail);
        snprintf(detail, sizeof(detail), "异常 %d 次", bad_mirror);
        check(bad_mirror == 0, "dir=-1 镜像满足 2π 互补", detail);
    }
}

/* ------------------------------------------------------------------ */
/* 4. 查表三角函数精度                                                  */
/* ------------------------------------------------------------------ */

static void test_sincos(void)
{
    const int kN = 200000;
    int i;
    float worst_sin = 0.0f;
    float worst_cos = 0.0f;
    char detail[96];

    printf("[4] Foc_SinCos：257 点表 + 线性插值精度\n");

    for (i = 0; i < kN; i++) {
        float angle = (-4.0f * TEST_TWO_PI) +
                      (8.0f * TEST_TWO_PI) * (float)i / (float)kN;
        float s;
        float c;
        float err;

        Foc_SinCos(angle, &s, &c);
        err = fabsf(s - sinf(angle));
        if (err > worst_sin) {
            worst_sin = err;
        }
        err = fabsf(c - cosf(angle));
        if (err > worst_cos) {
            worst_cos = err;
        }
    }

    snprintf(detail, sizeof(detail), "sin %.2e / cos %.2e",
             (double)worst_sin, (double)worst_cos);
    check((worst_sin < 2e-4f) && (worst_cos < 2e-4f), "全周（含越界）最大绝对误差", detail);
}

/* ------------------------------------------------------------------ */
/* 5. SVPWM / 零序注入                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief  单个 (A, θ) 下的占空比跨度与零序注入不变量
 * @param  magnitude  dq 矢量幅值（pu，1.0 = PWM 满幅）
 * @param  theta      矢量相位
 * @param  span_out   输出：max(duty) − min(duty)
 * @param  mid_out    输出：max(duty) + min(duty)
 * @param  clipped_out 输出：是否有任一相被钳位到 [0,1] 端点
 */
static void svpwm_probe(float magnitude, float theta,
                        float *span_out, float *mid_out, int *clipped_out)
{
    float duty_a;
    float duty_b;
    float duty_c;
    float duty_max;
    float duty_min;

    Foc_SvpwmOffsetOptimized(magnitude * cosf(theta), magnitude * sinf(theta),
                             &duty_a, &duty_b, &duty_c);

    duty_max = duty_a;
    duty_min = duty_a;
    if (duty_b > duty_max) {
        duty_max = duty_b;
    }
    if (duty_c > duty_max) {
        duty_max = duty_c;
    }
    if (duty_b < duty_min) {
        duty_min = duty_b;
    }
    if (duty_c < duty_min) {
        duty_min = duty_c;
    }

    *span_out = duty_max - duty_min;
    *mid_out = duty_max + duty_min;
    *clipped_out = ((duty_max >= 1.0f) || (duty_min <= 0.0f)) ? 1 : 0;
}

/**
 * @brief  在给定幅值下扫描一圈，取最大跨度
 */
static float svpwm_max_span(float magnitude, int *clipped_count)
{
    const int kN = 3600;
    int i;
    float worst_span = 0.0f;

    *clipped_count = 0;
    for (i = 0; i < kN; i++) {
        float theta = (float)i * TEST_TWO_PI / (float)kN;
        float span;
        float mid;
        int clipped;

        svpwm_probe(magnitude, theta, &span, &mid, &clipped);
        if (span > worst_span) {
            worst_span = span;
        }
        *clipped_count += clipped;
    }
    return worst_span;
}

static void test_svpwm(void)
{
    const int kN = 3600;
    int i;
    float worst_mid_error = 0.0f;
    char detail[128];

    printf("[5] SVPWM：零序注入不变量与线性区上限\n");

    /* 不变量：注入后必然有 max(duty) + min(duty) ≡ 1（而非"三者和 = 1.5"）。
       三者和仅在 (max+min)=0 的特殊相位等于 1.5，一般相位并不成立。 */
    for (i = 0; i < kN; i++) {
        float theta = (float)i * TEST_TWO_PI / (float)kN;
        float span;
        float mid;
        int clipped;
        float error;

        svpwm_probe(0.5f, theta, &span, &mid, &clipped);
        error = fabsf(mid - 1.0f);
        if (error > worst_mid_error) {
            worst_mid_error = error;
        }
    }
    snprintf(detail, sizeof(detail), "max |max(d)+min(d) − 1| = %.2e", (double)worst_mid_error);
    check(worst_mid_error < 1e-4f, "零序注入不变量 max(d)+min(d) ≡ 1", detail);

    /* 线性区：跨度应严格等于 √3·A，直到 A = 1/√3 */
    {
        const float kLinearMax = 1.0f / 1.7320508f;
        int clipped_050;
        int clipped_below;
        int clipped_above;
        float span_050 = svpwm_max_span(0.5f, &clipped_050);
        float span_below = svpwm_max_span(kLinearMax - 0.0005f, &clipped_below);
        float span_above = svpwm_max_span(kLinearMax + 0.0100f, &clipped_above);
        float target_050 = 1.7320508f * 0.5f;

        snprintf(detail, sizeof(detail), "A=0.50 跨度 %.4f（理论 √3·A = %.4f）clipped=%d",
                 (double)span_050, (double)target_050, clipped_050);
        check((fabsf(span_050 - target_050) < 1e-3f) && (clipped_050 == 0),
              "线性区：跨度 = √3·A 且不削顶", detail);

        snprintf(detail, sizeof(detail), "A=%.4f 跨度 %.4f clipped=%d",
                 (double)(kLinearMax - 0.0005f), (double)span_below, clipped_below);
        check(fabsf(span_below - (1.7320508f * (kLinearMax - 0.0005f))) < 1e-3f,
              "线性区上限内侧仍线性", detail);

        snprintf(detail, sizeof(detail), "A=%.4f 跨度 %.4f（理论 %.4f）clipped=%d",
                 (double)(kLinearMax + 0.0100f), (double)span_above,
                 (double)(1.7320508f * (kLinearMax + 0.0100f)), clipped_above);
        check((span_above < (1.7320508f * (kLinearMax + 0.0100f)) - 1e-3f) && (clipped_above > 0),
              "越限后跨度被压缩（进入过调制）", detail);
    }
}

/* ------------------------------------------------------------------ */
/* 6. 游标绝对位置解码                                                  */
/* ------------------------------------------------------------------ */

static void test_vernier(void)
{
    const int kN = 200000;
    int i;
    int rejected = 0;
    float worst_error = 0.0f;
    char detail[160];

    printf("[6] 游标解码：31 圈全窗口往返\n");

    for (i = 0; i < kN; i++) {
        float total = (TEST_TEETH_2 * (float)i) / (float)kN;
        uint16_t raw_motor;
        uint16_t raw_aux;
        float absolute;
        float error;

        synth_vernier(total, (int)CFG_ENC2_DIR, &raw_motor, &raw_aux);
        if (Foc_VernierRawAbs(raw_motor, raw_aux, &absolute) == 0U) {
            rejected++;
            continue;
        }
        error = fabsf(absolute - (total * TEST_TWO_PI));
        if (error > worst_error) {
            worst_error = error;
        }
    }

    snprintf(detail, sizeof(detail), "拒绝 %d/%d，最差误差 %.6f rad = %.4f°",
             rejected, kN, (double)worst_error, (double)(worst_error * 57.29578f));
    check((rejected == 0) && (worst_error < 0.001f), "全窗口无拒绝且误差 < 0.06°", detail);

    /* 未标定的旁置方向（CFG_ENC2_DIR 配反）：门限只应挡住一部分 */
    {
        int wrong_dir_rejected = 0;
        int accepted_whole_turn_error = 0;
        int accepted = 0;

        for (i = 0; i < 20000; i++) {
            float total = (TEST_TEETH_2 * (float)i) / 20000.0f;
            uint16_t raw_motor;
            uint16_t raw_aux;
            float absolute;
            float error_turns;

            synth_vernier(total, -(int)CFG_ENC2_DIR, &raw_motor, &raw_aux);
            if (Foc_VernierRawAbs(raw_motor, raw_aux, &absolute) == 0U) {
                wrong_dir_rejected++;
                continue;
            }
            accepted++;
            error_turns = (absolute - (total * TEST_TWO_PI)) / TEST_TWO_PI;
            if (fabsf(error_turns - floorf(error_turns + 0.5f)) < 0.1f) {
                accepted_whole_turn_error++;
            }
        }
        snprintf(detail, sizeof(detail),
                 "拒绝 %d/20000；被接受者中整圈错 %d/%d",
                 wrong_dir_rejected, accepted_whole_turn_error, accepted);
        check((wrong_dir_rejected > 0) && (wrong_dir_rejected < 20000),
              "方向配反：门限只能挡住一部分（须补方向自检）", detail);
    }

    /* 噪声免疫：单次旁置读数偏移 D 个计数 */
    {
        int shift;
        int first_slip = -1;

        for (shift = 1; (shift <= 4096) && (first_slip < 0); shift++) {
            int j;

            for (j = 0; j < 4000; j++) {
                float total = (TEST_TEETH_2 * (float)j) / 4000.0f;
                uint16_t raw_motor;
                uint16_t raw_aux;
                float absolute;

                synth_vernier(total, (int)CFG_ENC2_DIR, &raw_motor, &raw_aux);
                raw_aux = (uint16_t)((raw_aux + (uint16_t)shift) & (ENCODER_CPR - 1U));
                if (Foc_VernierRawAbs(raw_motor, raw_aux, &absolute) == 0U) {
                    continue;
                }
                if (fabsf((absolute - (total * TEST_TWO_PI)) / TEST_TWO_PI) > 0.5f) {
                    first_slip = shift;
                    break;
                }
            }
        }
        snprintf(detail, sizeof(detail), "最小滑移偏移 %d counts = %.2f°（旁置轴）",
                 first_slip, (first_slip > 0) ? ((360.0 * first_slip) / TEST_CPR) : 0.0);
        check((first_slip > 100) && (first_slip < 4096),
              "对随机噪声健壮：需 >100 counts 才可能滑移", detail);
    }
}

/* ------------------------------------------------------------------ */
/* 7. 电流环：量纲与母线电压增益调度                                    */
/* ------------------------------------------------------------------ */

static void test_current(void)
{
    Foc_Current_t current;
    char detail[160];

    printf("[7] 电流环：pu 量纲、限幅与 VBUS 增益调度\n");

    Foc_CurrentInit(&current, CFG_MOTOR_R_OHM, CFG_MOTOR_L_H, CFG_CUR_BW_HZ,
                    CFG_VBUS_NOMINAL_V, CFG_I_LIMIT_A, CFG_V_LIMIT);

    /* 零误差 → 零输出 */
    Foc_CurrentStep(&current, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, CFG_PWM_DT_S, 1U);
    snprintf(detail, sizeof(detail), "vd=%.4f vq=%.4f", (double)current.vd, (double)current.vq);
    check((fabsf(current.vd) < 1e-6f) && (fabsf(current.vq) < 1e-6f),
          "零误差下输出为零", detail);

    /* 大误差 → 必须被 v_lim 限制（pu 域，不是伏特） */
    {
        int k;
        float mag = 0.0f;

        for (k = 0; k < 2000; k++) {
            Foc_CurrentStep(&current, 0.0f, 0.0f, 0.0f, 3.0f, 20.0f,
                            CFG_PWM_DT_S, 1U);
            mag = sqrtf((current.vd * current.vd) + (current.vq * current.vq));
        }
        snprintf(detail, sizeof(detail), "|v|=%.4f  v_lim=%.4f", (double)mag, (double)CFG_V_LIMIT);
        check(mag <= (CFG_V_LIMIT + 1e-4f), "输出被圆形限幅约束在 v_lim（pu）", detail);
    }

    /* 母线电压调度：kp·vbus 与 ki·vbus 应为常数 */
    {
        float kp_at_24;
        float kp_at_48;
        float ki_at_24;
        float ki_at_48;

        Foc_CurrentSetVbus(&current, 24.0f);
        kp_at_24 = current.kp * 24.0f;
        ki_at_24 = current.ki * 24.0f;
        Foc_CurrentSetVbus(&current, 48.0f);
        kp_at_48 = current.kp * 48.0f;
        ki_at_48 = current.ki * 48.0f;

        snprintf(detail, sizeof(detail), "kp·Vbus: %.4f vs %.4f", (double)kp_at_24, (double)kp_at_48);
        check(fabsf(kp_at_24 - kp_at_48) < 1e-3f, "Kp 随 VBUS 反比调度（带宽不随母线变）", detail);

        snprintf(detail, sizeof(detail), "ki·Vbus: %.1f vs %.1f", (double)ki_at_24, (double)ki_at_48);
        check(fabsf(ki_at_24 - ki_at_48) < 1.0f, "Ki 随 VBUS 反比调度", detail);
    }

    /* 母线欠压回退：低于门限时用标称值，不应出现除零或畸变 */
    {
        float kp_low;

        Foc_CurrentSetVbus(&current, 1.0f);
        kp_low = current.kp * CFG_VBUS_NOMINAL_V;
        snprintf(detail, sizeof(detail), "Vbus=1V 时 kp·Vnom=%.4f", (double)kp_low);
        check(isfinite(kp_low) && (kp_low > 0.0f), "Vbus 低于门限时回退标称值", detail);
    }
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("YHorizon-JM FOC 纯算法层离线验证\n");
    printf("参考源码：../reference_src/Foc/（GPLv3，逐字复制）\n");
    printf("配置：p=%u  LSB/CPR=%u  游标 %u/%u  V_LIMIT=%.2f  I_LIMIT=%.1fA\n\n",
           (unsigned)CFG_POLE_PAIRS, (unsigned)ENCODER_CPR,
           (unsigned)CFG_VERNIER_TEETH1, (unsigned)CFG_VERNIER_TEETH2,
           (double)CFG_V_LIMIT, (double)CFG_I_LIMIT_A);

    Foc_MathInit();

    test_transforms();
    test_limit_dq();
    test_angle();
    test_sincos();
    test_svpwm();
    test_vernier();
    test_current();

    printf("\n结果：%d 项检查，%d 项失败\n", s_checks, s_failures);
    return (s_failures == 0) ? 0 : 1;
}
