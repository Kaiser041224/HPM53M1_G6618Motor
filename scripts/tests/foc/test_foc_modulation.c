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

    /* 过调制：限幅后保角（各相缩放比例一致）且占空比不越界 */
    {
        float d2[3];
        float scale2;
        float a, b;
        CHECK(foc_modulation_step(&cfg, 20.0f, 0.0f, 24.0f, d2, &scale2) == 0);
        CHECK(scale2 < 1.0f);
        CHECK(scale2 > 0.0f);
        for (int i = 0; i < 3; i++) {
            CHECK(d2[i] <= cfg.duty_max + 1e-5f);
            CHECK(d2[i] >= 1.0f - cfg.duty_max - 1e-5f);
        }
        /* 保角：限幅前后同一相偏离 0.5 的比例一致 */
        a = (d2[0] - 0.5f) / (d[0] - 0.5f);
        b = (d2[1] - 0.5f) / (d[1] - 0.5f);
        CHECK_NEAR(a, b, 1e-3f);
    }

    /* 母线过低 / 非有限输入 / duty_max 非法 → 拒绝 */
    CHECK(foc_modulation_step(&cfg, 1.0f, 0.0f, 8.0f, d, &scale) == -1);
    CHECK(foc_modulation_step(&cfg, 0.0f / 0.0f, 0.0f, 24.0f, d, &scale) == -1);
    CHECK(foc_modulation_step(&cfg, 1.0f, 0.0f, 1.0f / 0.0f, d, &scale) == -1);
    {
        foc_modulation_cfg_t bad = {.duty_max = 0.4f, .v_bus_min = 9.0f};
        CHECK(foc_modulation_step(&bad, 0.0f, 0.0f, 24.0f, d, &scale) == -1);
    }
}
