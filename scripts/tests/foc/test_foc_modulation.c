/**
 * @file    test_foc_modulation.c
 * @brief   foc_modulation 用例（零矢量 / 零序注入 / 采样窗口限幅 / 异常输入）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

#include "foc_modulation.h"

void test_foc_modulation(void) {
    foc_modulation_cfg_t cfg = {.duty_max = 0.885f, .v_bus_min = 9.0f};
    float d[3];
    float scale;

    /* 零矢量 → 三相 0.5 */
    CHECK(foc_modulation_step(&cfg, 0.0f, 0.0f, 24.0f, d, &scale) == 0);
    CHECK_NEAR(d[0], 0.5f, 1e-5f);
    CHECK_NEAR(d[1], 0.5f, 1e-5f);
    CHECK_NEAR(d[2], 0.5f, 1e-5f);
    CHECK_NEAR(scale, 1.0f, 1e-6f);

    /* 小信号：不缩放；零序注入性质 max+min = 1；不越采样窗口 */
    {
        float vmax, vmin;
        CHECK(foc_modulation_step(&cfg, 5.0f, 0.0f, 24.0f, d, &scale) == 0);
        CHECK_NEAR(scale, 1.0f, 1e-6f);
        vmax = (d[0] > d[1]) ? d[0] : d[1];
        vmax = (vmax > d[2]) ? vmax : d[2];
        vmin = (d[0] < d[1]) ? d[0] : d[1];
        vmin = (vmin < d[2]) ? vmin : d[2];
        CHECK_NEAR(vmax + vmin, 1.0f, 1e-5f);
        CHECK(vmax <= cfg.duty_max + 1e-6f);
        CHECK(vmin >= 1.0f - cfg.duty_max - 1e-6f);
    }

    /* 过调制：限幅后保角 + 幅值按 scale 缩放（由占空比重建 αβ 校验） */
    {
        const float v_bus = 24.0f;
        const float v_alpha = 15.0f;
        const float v_beta = -5.0f;
        float d2[3];
        float scale2;
        float alpha_hat, beta_hat, mag_hat, mag_in;
        CHECK(foc_modulation_step(&cfg, v_alpha, v_beta, v_bus, d2, &scale2) == 0);
        CHECK(scale2 < 1.0f);
        CHECK(scale2 > 0.0f);
        for (int i = 0; i < 3; i++) {
            CHECK(d2[i] <= cfg.duty_max + 1e-5f);
            CHECK(d2[i] >= 1.0f - cfg.duty_max - 1e-5f);
        }
        /* 重建：d = 0.5 + (v + 零序)/v_bus → (d−0.5)·v_bus 的 Clarke 变换 = v_αβ */
        alpha_hat = v_bus * (2.0f / 3.0f) * (d2[0] - 0.5f * (d2[1] + d2[2]));
        beta_hat = v_bus * (d2[1] - d2[2]) / FOC_SQRT3_F;
        mag_hat = sqrtf(alpha_hat * alpha_hat + beta_hat * beta_hat);
        mag_in = sqrtf(v_alpha * v_alpha + v_beta * v_beta);
        CHECK_NEAR(atan2f(beta_hat, alpha_hat), atan2f(v_beta, v_alpha), 1e-3f);
        CHECK_NEAR(mag_hat, mag_in * scale2, 1e-3f);
    }

    /* 幅度限幅阈值两侧：略低于 span_max 不缩放；略高于则缩放 */
    {
        float d3[3];
        float scale3;
        CHECK(foc_modulation_step(&cfg, 12.0f, 0.0f, 24.0f, d3, &scale3) == 0);
        CHECK_NEAR(scale3, 1.0f, 1e-6f);
        CHECK(foc_modulation_step(&cfg, 12.5f, 0.0f, 24.0f, d3, &scale3) == 0);
        CHECK(scale3 < 1.0f);
    }

    /* 母线过低 / 非有限输入 → 拒绝 */
    CHECK(foc_modulation_step(&cfg, 1.0f, 0.0f, 8.0f, d, &scale) == -1);
    CHECK(foc_modulation_step(&cfg, NAN, 0.0f, 24.0f, d, &scale) == -1);
    CHECK(foc_modulation_step(&cfg, 1.0f, 0.0f, INFINITY, d, &scale) == -1);

    /* NULL 参数 / v_scale_out 可空 */
    CHECK(foc_modulation_step(NULL, 0.0f, 0.0f, 24.0f, d, &scale) == -1);
    CHECK(foc_modulation_step(&cfg, 0.0f, 0.0f, 24.0f, NULL, &scale) == -1);
    CHECK(foc_modulation_step(&cfg, 1.0f, 0.5f, 24.0f, d, NULL) == 0);

    /* 配置非法：duty_max 越界/NaN/Inf；v_bus_min NaN/0/负 */
    {
        foc_modulation_cfg_t bad = {.duty_max = 0.4f, .v_bus_min = 9.0f};
        CHECK(foc_modulation_step(&bad, 0.0f, 0.0f, 24.0f, d, &scale) == -1);
        bad.duty_max = NAN;
        CHECK(foc_modulation_step(&bad, 0.0f, 0.0f, 24.0f, d, &scale) == -1);
        bad.duty_max = INFINITY;
        CHECK(foc_modulation_step(&bad, 0.0f, 0.0f, 24.0f, d, &scale) == -1);
        bad = (foc_modulation_cfg_t){.duty_max = 0.885f, .v_bus_min = NAN};
        CHECK(foc_modulation_step(&bad, 0.0f, 0.0f, 24.0f, d, &scale) == -1);
        bad.v_bus_min = 0.0f;
        CHECK(foc_modulation_step(&bad, 0.0f, 0.0f, 24.0f, d, &scale) == -1);
        bad.v_bus_min = -1.0f;
        CHECK(foc_modulation_step(&bad, 0.0f, 0.0f, 24.0f, d, &scale) == -1);
    }
}
