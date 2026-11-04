/**
 * @file    test_friction_math.c
 * @brief   摩擦模型求值与最小二乘拟合测试
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "algo_friction.h"
#include "test_util.h"

void test_friction_math(void) {
    algo_friction_params_t p = {
        .coulomb_nm = 1.0f, .viscous_nm_per_rad_s = 0.2f, .smoothing_rad_s = 0.05f};
    /* 零速无前馈；奇对称 */
    CHECK_NEAR(algo_friction_torque(&p, 0), 0, 1e-7f);
    CHECK_NEAR(algo_friction_torque(&p, -0.7f), -algo_friction_torque(&p, 0.7f), 1e-6f);
    /* 大速度：趋 Tc·sign + Kv·ω */
    CHECK_NEAR(algo_friction_torque(&p, 2.0f), 1.0f + 0.2f * 2.0f, 1e-6f);
    CHECK_NEAR(algo_friction_torque(&p, -2.0f), -1.0f - 0.2f * 2.0f, 1e-6f);
    /* 平滑区线性过零、±ω_s 处连续 */
    CHECK_NEAR(algo_friction_torque(&p, 0.025f), 1.0f * 0.5f + 0.2f * 0.025f, 1e-6f);
    CHECK_NEAR(algo_friction_torque(&p, 0.05f), algo_friction_torque(&p, 0.0501f), 1e-4f);
    /* 负速度不得给正前馈 */
    CHECK(algo_friction_torque(&p, -0.01f) < 0);
    /* 非法模型参数：平滑≤0 退化为硬符号，非有限输入返回0 */
    algo_friction_params_t hard = p;
    hard.smoothing_rad_s = 0;
    CHECK_NEAR(algo_friction_torque(&hard, 1e-6f), 1.0f + 0.2f * 1e-6f, 1e-6f);
    CHECK_NEAR(algo_friction_torque(&p, NAN), 0, 1e-7f);
    CHECK_NEAR(algo_friction_torque(NULL, 1.0f), 0, 1e-7f);
}

void test_friction_fit(void) {
    /* 完美数据恢复参数 */
    float speeds[8] = {0.1f, 0.3f, 0.8f, 1.5f, -0.1f, -0.3f, -0.8f, -1.5f};
    float torques[8];
    for (unsigned i = 0; i < 8; ++i)
        torques[i] = 0.9f * (speeds[i] > 0 ? 1.0f : -1.0f) + 0.15f * speeds[i];
    algo_friction_params_t out = {.smoothing_rad_s = 0.05f};
    float residual = -1;
    CHECK(algo_friction_fit(&out, &residual, speeds, torques, 8) == 0);
    CHECK_NEAR(out.coulomb_nm, 0.9f, 1e-5f);
    CHECK_NEAR(out.viscous_nm_per_rad_s, 0.15f, 1e-5f);
    CHECK_NEAR(residual, 0, 1e-5f);
    /* 噪声数据：参数近似、残差>0 */
    for (unsigned i = 0; i < 8; ++i)
        torques[i] += (i % 2) ? 0.02f : -0.02f;
    CHECK(algo_friction_fit(&out, &residual, speeds, torques, 8) == 0);
    CHECK_NEAR(out.coulomb_nm, 0.9f, 0.02f);
    CHECK(residual > 0 && residual < 0.05f);
    /* 单符号数据拒绝（无法分离 Tc/Kv 置信度） */
    float positive[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    float positive_t[4] = {1.0f, 1.1f, 1.2f, 1.3f};
    CHECK(algo_friction_fit(&out, &residual, positive, positive_t, 4) != 0);
    /* 点数不足 / NaN / 全零速度拒绝 */
    CHECK(algo_friction_fit(&out, &residual, speeds, torques, 3) != 0);
    torques[0] = NAN;
    CHECK(algo_friction_fit(&out, &residual, speeds, torques, 8) != 0);
    torques[0] = 0.9f;
    float zeros[6] = {0, 0, 0, 0, 0, 0};
    float zero_t[6] = {1, 1, 1, 1, 1, 1};
    CHECK(algo_friction_fit(&out, &residual, zeros, zero_t, 6) != 0);
    CHECK(algo_friction_fit(NULL, &residual, speeds, torques, 8) != 0);
}
