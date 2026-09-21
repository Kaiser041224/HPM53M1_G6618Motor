/**
 * @file    test_foc_angle.c
 * @brief   foc_angle 用例（电角度映射 / ωe 估计 / 异常输入）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

#include "foc_angle.h"

void test_foc_angle(void) {
    foc_angle_t ang;
    foc_angle_cfg_t cfg = {
        .pole_pairs = 10U,
        .direction = 1.0f,
        .offset_rad = 0.0f,
        .speed_lpf_hz = 100.0f,
        .sample_time_s = 1.0f / 25000.0f,
    };

    foc_angle_ctor(&ang);
    CHECK(ang.init(&ang, &cfg) == 0);

    /* 首拍：θe = p·θm，ωe = 0（无差分冲激） */
    {
        float omega = 123.0f;
        float theta_e = ang.step(&ang, 0.01f, &omega);
        CHECK_NEAR(theta_e, 0.1f, 1e-5f);
        CHECK_NEAR(omega, 0.0f, 1e-6f);
    }

    /* 恒定电角速度 → ωe 收敛（LPF 后接近真值） */
    {
        float omega = 0.0f;
        float theta_m = 0.01f;
        float omega_true = 100.0f; /* rad/s 电角速度 */
        for (int i = 0; i < 2000; i++) {
            theta_m += (omega_true / 10.0f) * cfg.sample_time_s;
            (void)ang.step(&ang, theta_m, &omega);
        }
        CHECK_NEAR(omega, omega_true, 0.5f);
    }

    /* 方向 −1：θe = −p·θm（归一化后） */
    {
        foc_angle_t a2;
        foc_angle_cfg_t c2 = cfg;
        float theta_e;
        c2.direction = -1.0f;
        foc_angle_ctor(&a2);
        CHECK(a2.init(&a2, &c2) == 0);
        theta_e = a2.step(&a2, 0.01f, NULL);
        CHECK_NEAR(theta_e, FOC_TWO_PI_F - 0.1f, 1e-4f);
    }

    /* 零点：offset = 0.1 rad → θe = p·θm − 0.1 */
    {
        foc_angle_t a3;
        foc_angle_cfg_t c3 = cfg;
        float theta_e;
        c3.offset_rad = 0.1f;
        foc_angle_ctor(&a3);
        CHECK(a3.init(&a3, &c3) == 0);
        theta_e = a3.step(&a3, 0.02f, NULL);
        CHECK_NEAR(theta_e, 0.1f, 1e-5f);
    }

    /* 异常输入（NaN）：保持上一拍输出 */
    {
        foc_angle_t a4;
        float omega = 0.0f;
        float t1, t2;
        foc_angle_ctor(&a4);
        CHECK(a4.init(&a4, &cfg) == 0);
        t1 = a4.step(&a4, 0.05f, &omega);
        t2 = a4.step(&a4, NAN, &omega);
        CHECK_NEAR(t2, t1, 1e-6f);
    }

    /* set_offset：运行中更新零点/方向 */
    {
        foc_angle_t a5;
        float theta_e;
        foc_angle_ctor(&a5);
        CHECK(a5.init(&a5, &cfg) == 0);
        (void)a5.step(&a5, 0.0f, NULL);
        a5.set_offset(&a5, FOC_PI_F, 1.0f);
        theta_e = a5.step(&a5, 0.0f, NULL);
        CHECK_NEAR(theta_e, FOC_PI_F, 1e-5f);
    }

    /* 方向 −1：θe 差分自动给出负 ωe */
    {
        foc_angle_t a7;
        foc_angle_cfg_t c7 = cfg;
        float omega = 0.0f;
        float theta_m = 0.0f;
        c7.direction = -1.0f;
        foc_angle_ctor(&a7);
        CHECK(a7.init(&a7, &c7) == 0);
        for (int i = 0; i < 2000; i++) {
            theta_m += (100.0f / 10.0f) * cfg.sample_time_s; /* 机械角正向 */
            (void)a7.step(&a7, theta_m, &omega);
        }
        CHECK_NEAR(omega, -100.0f, 0.5f);
    }

    /* reset 后重新起算：下一拍 ωe = 0（大跳变不产生尖峰） */
    {
        foc_angle_t a8;
        float omega = 0.0f;
        float theta_m = 0.0f;
        foc_angle_ctor(&a8);
        CHECK(a8.init(&a8, &cfg) == 0);
        for (int i = 0; i < 100; i++) {
            theta_m += 0.004f;
            (void)a8.step(&a8, theta_m, &omega);
        }
        CHECK(fabsf(omega) > 1.0f); /* 已有速度估计 */
        a8.reset(&a8);
        (void)a8.step(&a8, theta_m + 0.5f, &omega); /* 大跳变 */
        CHECK_NEAR(omega, 0.0f, 1e-6f);
    }

    /* set_offset 运行中调用：重新起算（ωe=0）且 θe 反映新零点 */
    {
        foc_angle_t a9;
        float omega = 0.0f;
        float theta_m = 0.0f;
        float theta_e;
        foc_angle_ctor(&a9);
        CHECK(a9.init(&a9, &cfg) == 0);
        for (int i = 0; i < 100; i++) {
            theta_m += 0.004f;
            (void)a9.step(&a9, theta_m, &omega);
        }
        a9.set_offset(&a9, FOC_PI_F, 1.0f);
        theta_e = a9.step(&a9, theta_m, &omega);
        CHECK_NEAR(omega, 0.0f, 1e-6f); /* 重起算，无尖峰 */
        CHECK_NEAR(theta_e, 4.0f - FOC_PI_F, 1e-3f); /* p·θm − π */
    }

    /* 非法配置：极点数为 0 / 非有限 / 负滤波 / 方向非法 */
    {
        foc_angle_t a6;
        foc_angle_cfg_t bad = cfg;
        foc_angle_ctor(&a6);
        bad.pole_pairs = 0U;
        CHECK(a6.init(&a6, &bad) == -1);
        bad = cfg;
        bad.sample_time_s = 0.0f;
        CHECK(a6.init(&a6, &bad) == -1);
        bad = cfg;
        bad.sample_time_s = NAN;
        CHECK(a6.init(&a6, &bad) == -1);
        bad = cfg;
        bad.speed_lpf_hz = -1.0f;
        CHECK(a6.init(&a6, &bad) == -1);
        bad = cfg;
        bad.offset_rad = NAN;
        CHECK(a6.init(&a6, &bad) == -1);
        bad = cfg;
        bad.direction = 0.5f;
        CHECK(a6.init(&a6, &bad) == -1);
    }
}
