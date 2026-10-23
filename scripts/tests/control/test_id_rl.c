/**
 * @file test_id_rl.c
 * @brief Rs/Ld 拟合和 PI 整定测试
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "id_rl.h"
#include "test_util.h"
void test_id_rl(void) {
    id_rl_sample_t alternating[128] = {0};
    float alternating_current = 3;
    for (unsigned i = 0; i < 128; ++i) {
        float v = 0.6f + ((i / 4U) % 2U ? -0.6f : 0.6f);
        alternating_current =
            (alternating_current - v / 0.2f) * expf(-0.2f * 40e-6f / 120e-6f) + v / 0.2f;
        alternating[i] =
            (id_rl_sample_t){.i_d_a = alternating_current, .v_d_previous_v = v, .valid = true};
    }
    id_rl_fit_t alternating_fit;
    CHECK(id_rl_pulse_inductance(alternating, 128, 40e-6f, 0.2f, &alternating_fit) == 0);
    CHECK_NEAR(alternating_fit.value, 120e-6f, 6e-6f);
    CHECK(id_rl_pulse_inductance(alternating, 128, 0, 0.2f, &alternating_fit) != 0);
    alternating[50].valid = false;
    CHECK(id_rl_pulse_inductance(alternating, 128, 40e-6f, 0.2f, &alternating_fit) != 0);
    alternating[50].valid = true;
    /* 恒压偏差与16次平均后残余噪声；按电压极性而非噪声增量符号分组。 */
    alternating_current = 3;
    uint32_t pulse_random = 321;
    for (unsigned i = 0; i < 128; ++i) {
        float v = 0.7f + ((i / 8U) % 2U ? -0.6f : 0.6f);
        float equilibrium = (v - 0.1f) / 0.2f;
        alternating_current =
            (alternating_current - equilibrium) * expf(-0.2f * 40e-6f / 120e-6f) + equilibrium;
        float noise = 0;
        for (unsigned repeat = 0; repeat < 16; ++repeat) {
            pulse_random = pulse_random * 1664525U + 1013904223U;
            noise += ((float)(pulse_random >> 8) / 16777216.0f - 0.5f) * 0.8f / 16;
        }
        alternating[i] = (id_rl_sample_t){
            .i_d_a = alternating_current + noise, .v_d_previous_v = v, .valid = true};
    }
    CHECK(id_rl_pulse_inductance(alternating, 128, 40e-6f, 0.2f, &alternating_fit) == 0);
    CHECK_NEAR(alternating_fit.value, 120e-6f, 25e-6f);
    /* 单点差分残差不应代表段积分估计质量：0.12A交替采样污染。 */
    for (unsigned i = 0; i < 128; ++i)
        alternating[i].i_d_a += (i % 2U ? 0.12f : -0.12f);
    CHECK(id_rl_pulse_inductance(alternating, 128, 40e-6f, 0.2f, &alternating_fit) == 0);
    CHECK_NEAR(alternating_fit.value, 120e-6f, 25e-6f);
    for (unsigned i = 0; i < 128; ++i)
        alternating[i].i_d_a -= (i % 2U ? 0.12f : -0.12f);
    /* 非共同、段内缓慢漂移不能被端点均值隐藏，必须拒绝。 */
    for (unsigned i = 0; i < 128; ++i)
        alternating[i].i_d_a += ((i / 8U) % 3U == 0 ? 0.3f : -0.15f) * (i % 8U);
    CHECK(id_rl_pulse_inductance(alternating, 128, 40e-6f, 0.2f, &alternating_fit) == -4);
    CHECK(alternating_fit.residual > 0.15f);
    id_rl_sample_t samples[128] = {0};
    id_rl_fit_t fit;
    float current[5] = {1, 2, 3, 2, 1}, voltage[5], kp, ki;
    for (unsigned i = 0; i < 5; ++i)
        voltage[i] = 0.2f * current[i] + 0.12f;
    CHECK(id_rl_resistance(current, voltage, 5, &fit) == 0);
    CHECK_NEAR(fit.value, 0.2f, 1e-5f);
    CHECK_NEAR(fit.intercept, 0.12f, 1e-5f);
    CHECK(id_rl_resistance(current, voltage, 1, &fit) != 0);
    float same[5] = {1, 1, 1, 1, 1};
    CHECK(id_rl_resistance(same, voltage, 5, &fit) != 0);
    float invalid[5] = {1, 2, NAN, 2, 1};
    CHECK(id_rl_resistance(invalid, voltage, 5, &fit) != 0);
    float state = 2;
    const float resistance = 0.2f, inductance = 120e-6f, dt = 40e-6f;
    for (unsigned i = 0; i < 128; ++i) {
        float v = i < 64 ? 0.62f : 0.52f;
        state = (state - (v - 0.12f) / resistance) * expf(-resistance * dt / inductance)
              + (v - 0.12f) / resistance;
        samples[i].i_d_a = state;
        samples[i].v_d_previous_v = v;
        samples[i].valid = true;
    }
    CHECK(id_rl_inductance(samples, 128, dt, resistance, &fit) == 0);
    CHECK_NEAR(fit.value, inductance, 2e-6f);
    samples[50].valid = false;
    CHECK(id_rl_inductance(samples, 128, dt, resistance, &fit) != 0);
    for (unsigned i = 0; i < 128; ++i) {
        samples[i].valid = true;
        samples[i].i_d_a = 2;
        samples[i].v_d_previous_v = 0.4f;
    }
    CHECK(id_rl_inductance(samples, 128, dt, resistance, &fit) != 0);
    CHECK(id_rl_pi(resistance, inductance, 500, &kp, &ki) == 0);
    CHECK_NEAR(kp, 0.376991f, 1e-5f);
    CHECK_NEAR(ki, 628.3185f, 0.01f);
    CHECK(id_rl_pi(-1, inductance, 500, &kp, &ki) != 0);
    CHECK(id_rl_pi(resistance, inductance, NAN, &kp, &ki) != 0);
    id_rl_step_result_t step;
    for (unsigned i = 0; i < 128; ++i) {
        samples[i].valid = true;
        samples[i].i_d_a = 2.5f - 0.5f * expf(-(float)i * dt * 2000);
    }
    CHECK(id_rl_step(samples, 128, dt, 2, 2.5f, &step) == 0);
    CHECK(step.passed);
    CHECK(step.rise_s > 0 && step.rise_s < 0.002f);

    /* 单个 q 轴采样尖峰不应否决整个阶跃；q 轴角度错误会表现为均值偏差。 */
    samples[10].i_q_a = 0.5f;
    CHECK(id_rl_step(samples, 128, dt, 2, 2.5f, &step) == 0);
    CHECK(step.passed);
    samples[10].i_q_a = 0.0f;

    /* 采样纹波不应把已稳定的阶跃判成失败：尾段峰峰值可超过 0.1A，
     * 但 RMS 低于 0.1A，且平滑后超调仍小于 20%。 */
    for (unsigned i = 0; i < 128; ++i) {
        float ripple = (i % 4U == 0U) ? 0.08f : ((i % 4U == 2U) ? -0.08f : 0.0f);
        samples[i].i_d_a = 2.5f - 0.5f * expf(-(float)i * dt * 2000) + ripple;
    }
    CHECK(id_rl_step(samples, 128, dt, 2, 2.5f, &step) == 0);
    CHECK(step.passed);
    CHECK(step.ripple_a > 0.10f);
    CHECK(step.ripple_rms_a < 0.10f);
    CHECK(step.overshoot <= 0.20f);

    /* 单点尖峰可以被平滑成低超调，但 raw 必须仍能否决。 */
    for (unsigned i = 0; i < 128; ++i)
        samples[i].i_d_a = 2.5f - 0.5f * expf(-(float)i * dt * 2000);
    samples[120].i_d_a = 2.7f; /* raw 40%，3点平滑约 13% */
    CHECK(id_rl_step(samples, 128, dt, 2, 2.5f, &step) == 0);
    CHECK(step.overshoot < 0.20f);
    CHECK(step.overshoot_raw > 0.30f);
    CHECK(!step.passed);

    samples[120].i_d_a = 4;
    CHECK(id_rl_step(samples, 128, dt, 2, 2.5f, &step) == 0);
    CHECK(!step.passed);
    /* 恒定偏差不是峰峰纹波：尾段恒3A，对2.5A目标误差0.5，纹波应0。 */
    for (unsigned i=0;i<128;++i) samples[i].i_d_a=3.0f;
    CHECK(id_rl_step(samples,128,dt,2,2.5f,&step)==0);
    CHECK_NEAR(step.ripple_a,0,1e-6f);
    CHECK_NEAR(step.error_a,0.5f,1e-6f);
    /* 有限但偏大的q残差是诊断失败，不是无法计算指标。 */
    for (unsigned i = 0; i < 128; ++i)
        samples[i].i_q_a = 0.3f;
    CHECK(id_rl_step(samples, 128, dt, 2, 2.5f, &step) == 0);
    CHECK(!step.passed);
    for (unsigned i = 0; i < 128; ++i)
        samples[i].i_q_a = 0;
    /* 多次对齐平均后的暂态仍有残余噪声，电感估计不应因差分回归产生大偏差。 */
    state = 3;
    uint32_t random_state = 123;
    for (unsigned i = 0; i < 128; ++i) {
        state = (state - 3.5f) * expf(-resistance * dt / inductance) + 3.5f;
        float noise = 0;
        for (unsigned repeat = 0; repeat < 16; ++repeat) {
            random_state = random_state * 1664525U + 1013904223U;
            noise += ((float)(random_state >> 8) / 16777216.0f - 0.5f) * 0.8f / 16;
        }
        samples[i] =
            (id_rl_sample_t){.i_d_a = state + noise, .v_d_previous_v = 0.7f, .valid = true};
    }
    CHECK(id_rl_inductance(samples, 128, dt, resistance, &fit) == 0);
    CHECK_NEAR(fit.value, inductance, 20e-6f);
}
