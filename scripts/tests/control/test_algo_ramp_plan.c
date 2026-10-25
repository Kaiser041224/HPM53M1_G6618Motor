/**
 * @file    test_algo_ramp_plan.c
 * @brief   梯形轨迹有符号初速度、Yd/Ydd 与目标连续性契约测试
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "algo_ramp.h"
#include "test_util.h"

/** @brief 构造 1 rad/s、10 rad/s² 的梯形轨迹 */
static void ramp_setup(algo_ramp_t* ramp) {
    algo_ramp_cfg_t cfg = {
        .mode = ALGO_RAMP_MODE_TRAPEZOIDAL,
        .rate = 1.0f,
        .step_time_s = 1.0f / 15000.0f,
        .target = 0.0f,
        .initial = 0.0f,
        .accel_max = 10.0f,
        .duration_s = 0.0f,
    };

    algo_ramp_ctor(ramp);
    CHECK(ramp->init(ramp, &cfg) == 0);
}

void test_algo_ramp_plan(void) {
    algo_ramp_t ramp;

    /* 1) plan 保留有符号初速度，首拍按限幅加速，而不是从 0 重新起步。 */
    ramp_setup(&ramp);
    CHECK(ramp.plan(&ramp, 1.0f, 0.0f, 0.5f) == 0);
    CHECK_NEAR(ramp.get_velocity(&ramp), 0.5f, 1e-6f);
    CHECK_NEAR(ramp.get_acceleration(&ramp), 0.0f, 1e-6f);
    (void)ramp.step(&ramp);
    CHECK(ramp.get_velocity(&ramp) > 0.5f);
    CHECK_NEAR(ramp.get_acceleration(&ramp), 10.0f, 1e-3f);

    /* 2) 正转后反向：必须先制动到 0，再反向加速，终点不得越过目标。 */
    ramp_setup(&ramp);
    CHECK(ramp.plan(&ramp, -1.0f, 0.0f, 1.0f) == 0);
    (void)ramp.step(&ramp);
    CHECK_NEAR(ramp.get_acceleration(&ramp), -10.0f, 1e-3f);
    for (unsigned i = 0; (i < 25000U) && !ramp.is_done(&ramp); ++i) {
        float position = ramp.step(&ramp);
        CHECK(position >= -1.0001f);
    }
    CHECK_NEAR(ramp.get_current(&ramp), -1.0f, 1e-4f);
    CHECK_NEAR(ramp.get_velocity(&ramp), 0.0f, 1e-4f);
    CHECK_NEAR(ramp.get_acceleration(&ramp), 0.0f, 1e-4f);

    /* 3) 运动中更新目标：速度连续，不能回到 0；下一拍重新计算 Ydd。 */
    ramp_setup(&ramp);
    CHECK(ramp.plan(&ramp, 1.0f, 0.0f, 0.0f) == 0);
    for (unsigned i = 0; i < 100U; ++i) {
        (void)ramp.step(&ramp);
    }
    float velocity_before = ramp.get_velocity(&ramp);
    CHECK(velocity_before > 0.05f);
    CHECK(ramp.set_target(&ramp, 2.0f) == 0);
    CHECK_NEAR(ramp.get_velocity(&ramp), velocity_before, 1e-6f);
    CHECK_NEAR(ramp.get_acceleration(&ramp), 0.0f, 1e-6f);

    /* 4) v_max 钳位后实际 Ydd=0；不得把命令加速度送进惯量前馈。 */
    ramp_setup(&ramp);
    CHECK(ramp.plan(&ramp, 100.0f, 0.0f, 0.0f) == 0);
    for (unsigned i = 0; (i < 3000U) && (ramp.get_velocity(&ramp) < 1.0f); ++i) {
        (void)ramp.step(&ramp);
    }
    /* 到达 v_max 后再走一拍：钳位在线，实际 Ydd 必须为 0。 */
    (void)ramp.step(&ramp);
    CHECK_NEAR(ramp.get_velocity(&ramp), 1.0f, 1e-3f);
    CHECK_NEAR(ramp.get_acceleration(&ramp), 0.0f, 1e-6f);

    /* 5) 非法初速度/目标必须拒绝，且不改动已有轨迹状态。 */
    float position_before_invalid = ramp.get_current(&ramp);
    CHECK(ramp.plan(&ramp, NAN, 0.0f, 0.0f) != 0);
    CHECK(ramp.plan(&ramp, 1.0f, INFINITY, 0.0f) != 0);
    CHECK_NEAR(ramp.get_current(&ramp), position_before_invalid, 1e-6f);
}
