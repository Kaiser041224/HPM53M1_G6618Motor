/** @file test_algo_speed_observer.c
 * @brief 30kHz 转子速度观测器（PLL）纯算法测试
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "algo_speed_observer.h"
#include "test_util.h"

#include <math.h>

/** @brief 以固定机械角速度运行 n 拍 */
static void run_constant(algo_speed_observer_t* obs, float omega, float ts, int n) {
    float theta = 0.0f;
    float out = 0.0f;
    (void)obs->step(obs, theta, ts, &out); /* prime */
    for (int i = 0; i < n; ++i) {
        theta += omega * ts;
        (void)obs->step(obs, theta, ts, &out);
    }
}

void test_algo_speed_observer(void) {
    algo_speed_observer_t obs;
    algo_speed_observer_cfg_t cfg = {
        .bandwidth_rad_s = 500.0f,
        .sample_time_s = 1.0f / 30000.0f,
        .max_speed_rad_s = 700.0f,
    };

    algo_speed_observer_ctor(&obs);
    CHECK(obs.init(&obs, &cfg) == 0);

    /* 首拍：锁相到测量角，速度为 0 */
    {
        float w = 123.0f;
        (void)obs.step(&obs, 0.1f, cfg.sample_time_s, &w);
        CHECK_NEAR(w, 0.0f, 1e-6f);
        CHECK_NEAR(obs.get_theta(&obs), 0.1f, 1e-5f);
        CHECK_NEAR(obs.get_omega(&obs), 0.0f, 1e-6f);
    }

    /* 恒定机械速度 10 rad/s（500rpm 电机，50:1 -> 10rpm 输出） */
    {
        obs.reset(&obs);
        run_constant(&obs, 10.0f, cfg.sample_time_s, 2000);
        CHECK_NEAR(obs.get_omega(&obs), 10.0f, 0.5f);
    }

    /* 1rpm 输出（50:1 -> 5.236 rad/s 机械）低速收敛 */
    {
        obs.reset(&obs);
        run_constant(&obs, 5.236f, cfg.sample_time_s, 2000);
        CHECK_NEAR(obs.get_omega(&obs), 5.236f, 0.1f);
    }

    /* 反向 -10 rad/s */
    {
        obs.reset(&obs);
        run_constant(&obs, -10.0f, cfg.sample_time_s, 2000);
        CHECK_NEAR(obs.get_omega(&obs), -10.0f, 0.5f);
    }

    /* 3x dt 抖动：仍收敛，不产生尖峰 */
    {
        float theta = 0.0f;
        float w = 0.0f;
        obs.reset(&obs);
        (void)obs.step(&obs, 0.0f, cfg.sample_time_s, &w);
        for (int i = 0; i < 2000; ++i) {
            float dt = ((i % 5) == 4) ? 3.0f * cfg.sample_time_s : cfg.sample_time_s;
            theta += 10.0f * dt;
            (void)obs.step(&obs, theta, dt, &w);
        }
        CHECK_NEAR(w, 10.0f, 0.5f);
        CHECK(fabsf(w) < 100.0f);
    }

    /* 非有限角度 / 非法 dt：保持上一拍速度 */
    {
        float w = 10.0f;
        (void)obs.step(&obs, 0.2f, cfg.sample_time_s, &w);
        float held = obs.get_omega(&obs);
        (void)obs.step(&obs, NAN, cfg.sample_time_s, &w);
        CHECK_NEAR(w, held, 1e-6f);
        (void)obs.step(&obs, 0.2f, 0.0f, &w);
        CHECK_NEAR(w, held, 1e-6f);
        (void)obs.step(&obs, 0.2f, 1.0e-2f, &w);
        CHECK_NEAR(w, held, 1e-6f);
    }

    /* max_speed 限幅 */
    {
        float theta = 0.0f;
        float w = 0.0f;
        obs.reset(&obs);
        (void)obs.step(&obs, 0.0f, cfg.sample_time_s, &w);
        for (int i = 0; i < 4000; ++i) {
            theta += 800.0f * cfg.sample_time_s;
            (void)obs.step(&obs, theta, cfg.sample_time_s, &w);
        }
        CHECK(fabsf(w) <= cfg.max_speed_rad_s + 1e-3f);
        CHECK(w > 100.0f);
    }

    /* 停止后速度渐近衰减到 0（无 snap 门限） */
    {
        obs.reset(&obs);
        run_constant(&obs, 10.0f, cfg.sample_time_s, 2000);
        float theta = 10.0f * cfg.sample_time_s * 2000.0f;
        float w = 0.0f;
        for (int i = 0; i < 3000; ++i) {
            (void)obs.step(&obs, theta, cfg.sample_time_s, &w);
        }
        CHECK(fabsf(w) < 0.1f);
    }

    /* reset 后首拍速度为 0 */
    {
        float w = 10.0f;
        obs.reset(&obs);
        (void)obs.step(&obs, 0.3f, cfg.sample_time_s, &w);
        CHECK_NEAR(w, 0.0f, 1e-6f);
        CHECK_NEAR(obs.get_theta(&obs), 0.3f, 1e-5f);
    }

    /* 非法配置 */
    {
        algo_speed_observer_t bad;
        algo_speed_observer_cfg_t c = cfg;
        algo_speed_observer_ctor(&bad);
        c.bandwidth_rad_s = 0.0f;
        CHECK(bad.init(&bad, &c) != 0);
        c = cfg;
        c.bandwidth_rad_s = NAN;
        CHECK(bad.init(&bad, &c) != 0);
        c = cfg;
        c.sample_time_s = 0.0f;
        CHECK(bad.init(&bad, &c) != 0);
        c = cfg;
        c.max_speed_rad_s = 0.0f;
        CHECK(bad.init(&bad, &c) != 0);
    }
}
