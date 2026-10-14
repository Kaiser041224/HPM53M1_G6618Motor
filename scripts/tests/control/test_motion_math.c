/** @file test_motion_math.c
 * @brief 游标圈数与速度控制数学测试
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "algo_motion.h"
#include "test_util.h"
void test_motion_math(void) {
    for (unsigned n = 0; n < 50; ++n) {
        float x = n + 0.37f;
        float u = 0.37f, v = fmodf(.98f * x + .007f, 1);
        float q;
        CHECK(algo_motion_vernier(u, v, .007f, &q) == 0);
        CHECK_NEAR(q, x * 6.283185307f / 50, 1e-5f);
    }
    float q;
    CHECK(algo_motion_vernier(.2f, .213f, .007f, &q) != 0);
    algo_motion_pi_t pi = {0};
    CHECK_NEAR(algo_motion_speed(&pi, 1, 0, 2, 10, .0001f, 1), 1, 1e-6f);
    CHECK_NEAR(pi.integral, 0, 1e-6f);
    CHECK(algo_motion_speed(&pi, -1, 0, 2, 10, .0001f, 1) < 0);
    pi.integral = NAN;
    CHECK_NEAR(algo_motion_speed(&pi, 0, 0, 1, 1, .0001f, 1), 0, 1e-6f);
    CHECK_NEAR(pi.integral, 0, 1e-6f);
    CHECK(algo_motion_vernier(.2f, .226f, .03f, &q) != 0);
    CHECK(algo_motion_vernier(.2f, INFINITY, .01f, &q) != 0);
    CHECK(algo_motion_vernier(NAN, .1f, .01f, &q) != 0);
    pi.integral = 0;
    CHECK_NEAR(algo_motion_speed(&pi, 1e30f, 0, 1e30f, 0, .0001f, 1), 0, 1e-6f);
    CHECK(algo_motion_vernier(1e20f, .1f, .01f, &q) != 0);
    CHECK(algo_motion_vernier(.1f, -.1f, .01f, &q) != 0);
    CHECK(algo_motion_vernier(1.37f, .3696f, .007f, &q) != 0);
    const float turns[] = {0.0f, .9999f, 1.0001f, 49.9999f};
    for (unsigned i = 0; i < 4; ++i) {
        float u = fmodf(turns[i], 1), v = fmodf(.98f * turns[i] + .007f, 1);
        CHECK(algo_motion_vernier(u, v, .007f, &q) == 0);
        CHECK_NEAR(q, turns[i] * 6.283185307f / 50, 1e-5f);
    }
}
