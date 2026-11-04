/** @file test_algo_continuous.c
 * @brief 连续角 accumulate/unwrap 纯函数测试（跨 0/2π 边界、跳变、重基准）
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "algo_motion.h"
#include "foc_math.h"
#include "test_util.h"

#define MAX_DELTA 0.5f

void test_algo_continuous(void) {
    {
        /* 正常小步进累积 */
        float cont = 0.0f;
        bool valid = false;
        bool rebased = false;

        CHECK(algo_motion_continuous_step(0.0f, 0.0f, MAX_DELTA, &cont, &valid, &rebased) == 0);
        CHECK(valid);
        CHECK(rebased);
        CHECK_NEAR(cont, 0.0f, 1e-6f);

        for (int i = 1; i <= 10; ++i) {
            float raw = 0.1f * (float)i;

            CHECK(algo_motion_continuous_step(raw, 0.1f * (float)(i - 1), MAX_DELTA, &cont, &valid,
                                              &rebased)
                  == 0);
            CHECK(!rebased);
        }
        CHECK_NEAR(cont, 1.0f, 1e-5f);
    }
    {
        /* 跨 0/2π 边界（正向）：6.28 → 0.01 应累积 +0.013，而不是 -6.27 */
        float cont = 6.0f;
        bool valid = true;
        bool rebased = false;

        CHECK(algo_motion_continuous_step(0.01f, 6.27f, MAX_DELTA, &cont, &valid, &rebased) == 0);
        CHECK(valid);
        CHECK_NEAR(cont, 6.0f + 0.0231853f, 1e-4f);
        CHECK(cont > 6.0f);
    }
    {
        /* 跨边界（反向）：0.01 → 6.27 应累积 -0.023 */
        float cont = 6.0f;
        bool valid = true;
        bool rebased = false;

        CHECK(algo_motion_continuous_step(6.27f, 0.01f, MAX_DELTA, &cont, &valid, &rebased) == 0);
        CHECK_NEAR(cont, 6.0f - 0.0231853f, 1e-4f);
    }
    {
        /* 跳变：|delta| ≥ 0.5 → 拒绝累积、valid=0、不重基准 */
        float cont = 3.0f;
        bool valid = true;
        bool rebased = false;

        CHECK(algo_motion_continuous_step(3.4f, 3.0f, MAX_DELTA, &cont, &valid, &rebased) == 0);
        CHECK_NEAR(cont, 3.4f, 1e-6f);
        /* 下一次大跳变 */
        cont = 3.4f;
        CHECK(algo_motion_continuous_step(0.5f, 3.4f, MAX_DELTA, &cont, &valid, &rebased) == -1);
        CHECK(!valid);
        CHECK_NEAR(cont, 3.4f, 1e-6f); /* 本拍不改累积值 */
    }
    {
        /* 跳变后重基准：连续角回到当前单圈角，圈数不继承 */
        float cont = 99.0f;
        bool valid = false;
        bool rebased = false;

        CHECK(algo_motion_continuous_step(1.25f, 0.0f, MAX_DELTA, &cont, &valid, &rebased) == 0);
        CHECK(valid);
        CHECK(rebased);
        CHECK_NEAR(cont, 1.25f, 1e-6f);
    }
    {
        /* 非法参数 */
        float cont = 0.0f;
        bool valid = true;

        CHECK(algo_motion_continuous_step(0.0f, 0.0f, 0.0f, &cont, &valid, NULL) == -2);
        CHECK(algo_motion_continuous_step(0.0f, 0.0f, MAX_DELTA, NULL, &valid, NULL) == -2);
        CHECK(algo_motion_continuous_step(0.0f, 0.0f, MAX_DELTA, &cont, NULL, NULL) == -2);
    }
    {
        /* 长行程：跨 7 个 2π 边界仍单调（每次 0.4 rad，逐步走 45 rad） */
        float cont = 0.0f;
        float raw_prev = 0.0f;
        bool valid = false;
        bool rebased = false;

        CHECK(algo_motion_continuous_step(0.0f, 0.0f, MAX_DELTA, &cont, &valid, &rebased) == 0);
        for (int i = 1; i <= 120; ++i) {
            float raw = foc_wrap_2pi(0.4f * (float)i);

            CHECK(algo_motion_continuous_step(raw, raw_prev, MAX_DELTA, &cont, &valid, &rebased)
                  == 0);
            CHECK(valid);
            raw_prev = raw;
        }
        CHECK_NEAR(cont, 48.0f, 0.03f);
    }
}

/**
 * @brief 位置 P：单圈最短路径 vs 连续坐标
 *
 * 台架缺陷（2026-10-27）：多圈模式误用单圈 P，整圈误差被折成 0 →
 * `pos_multi 400` 从 0 出发走到 400 后（差 0）、或从 40 出发差 360 时静止。
 * 本用例把两种语义钉死，防止再退化。
 */
void test_algo_position_p(void) {
    const float two_pi = 6.28318530718f;

    /* 差恰好一整圈：单圈版本 → 0（这就是"差整圈即停"），连续版本 → 满值 */
    CHECK_NEAR(algo_motion_position(0.5f + two_pi, 0.5f, 10.0f, 0.0f), 0.0f, 1e-5f);
    CHECK_NEAR(
        algo_motion_position_cont(0.5f + two_pi, 0.5f, 10.0f, 0.0f), 10.0f * two_pi, 1e-4f);

    /* 差 1.5 圈：单圈版本折成 ±π 边界（wrap_pm_pi 归入 +π 侧），连续版本保留 3π */
    CHECK_NEAR(algo_motion_position(0.5f + 1.5f * two_pi, 0.5f, 1.0f, 0.0f), 3.14159265f,
               1e-4f);
    CHECK_NEAR(algo_motion_position_cont(0.5f + 1.5f * two_pi, 0.5f, 1.0f, 0.0f),
               1.5f * two_pi, 1e-4f);

    /* 速度前馈叠加与增益极性 */
    CHECK_NEAR(algo_motion_position_cont(1.0f, 0.0f, 10.0f, 2.0f), 12.0f, 1e-5f);
    CHECK_NEAR(algo_motion_position_cont(0.0f, 1.0f, 10.0f, -1.0f), -11.0f, 1e-5f);
}
