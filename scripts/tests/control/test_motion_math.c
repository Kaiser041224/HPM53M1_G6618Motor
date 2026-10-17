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

/** @brief 速度PI条件积分：外部限幅冻结积分，误差反向仍允许回退 */
void test_motion_speed_anti_windup(void) {
    algo_motion_pi_t pi = {0};
    pi.saturated = true;
    float out = algo_motion_speed(&pi, 1, 0, 0, 10, .0001f, 10);
    CHECK_NEAR(pi.integral, 0, 1e-9f); /* 饱和且误差持续：不积分 */
    CHECK_NEAR(out, 0, 1e-6f);         /* kp=0 时输出=积分 */
    pi.integral = 1;
    out = algo_motion_speed(&pi, -1, 0, 0, 10, .0001f, 10);
    CHECK(pi.integral < 1);            /* 误差反向=回退方向：仍允许积分 */
    CHECK(out < 1);
    pi.saturated = false;
    float before = pi.integral;
    (void)algo_motion_speed(&pi, 1, 0, 0, 10, .0001f, 10);
    CHECK(pi.integral > before);       /* 非饱和恢复条件积分 */
}

/** @brief ODrive POS_FILTER 二阶临界阻尼滤波（5kHz 拍语义） */
void test_motion_pos_filter(void) {
    algo_motion_pos_filter_t f = {0};
    float dt = 2e-4f;
    /* 阶跃 0→1：临界阻尼无超调，1s(=5/bw)内收敛 */
    for (unsigned i = 0; i < 5000; ++i) {
        algo_motion_pos_filter_step(&f, 1.0f, 0, 10.0f, dt);
        CHECK(f.pos_setpoint_rad <= 1.0001f);
    }
    CHECK_NEAR(f.pos_setpoint_rad, 1.0f, 0.01f);
    CHECK_NEAR(f.vel_setpoint_rad_s, 0, 0.01f);
    /* input_vel=输入轨迹速度估计（ODrive 语义）：斜坡跟踪滞后被消除 */
    algo_motion_pos_filter_t g = {0};
    for (unsigned i = 0; i < 5000; ++i) {
        float input_pos = 0.5f * (float)(i + 1) * dt;        /* 0.5 rad/s 斜坡 */
        algo_motion_pos_filter_step(&g, input_pos, 0.5f, 10.0f, dt);
    }
    CHECK_NEAR(g.vel_setpoint_rad_s, 0.5f, 0.01f);
    CHECK_NEAR(g.pos_setpoint_rad, 0.5f * 5000 * dt, 0.01f); /* 滞后≈0 */
    /* 恒定 input_pos + input_vel 失配：pos_sp 带 2*vin/bw 静差、vel_sp 归 0 */
    algo_motion_pos_filter_t h = {0};
    for (unsigned i = 0; i < 5000; ++i)
        algo_motion_pos_filter_step(&h, 1.0f, 0.5f, 10.0f, dt);
    CHECK_NEAR(h.pos_setpoint_rad, 1.1f, 0.01f); /* = input_pos + 2*vin/bw */
    CHECK_NEAR(h.vel_setpoint_rad_s, 0, 0.01f);
}

/** @brief 带宽钳位≤0.25/dt（ODrive 同源约束）与 bw≤0 直通 */
void test_motion_pos_filter_bw_clamp(void) {
    algo_motion_pos_filter_t f = {0};
    float dt = 2e-4f; /* 0.25/dt = 1250 */
    algo_motion_pos_filter_step(&f, 1.0f, 0, 2000.0f, dt);
    /* 钳位到1250：单拍 pos_sp = dt*(1250^2)*dt = 0.0625；未钳位则 0.16 */
    CHECK_NEAR(f.pos_setpoint_rad, 0.0625f, 1e-6f);
    CHECK_NEAR(f.vel_setpoint_rad_s, 312.5f, 1e-3f); /* = dt*1250^2 */
    /* bw≤0 直通 */
    algo_motion_pos_filter_t g = {0};
    algo_motion_pos_filter_step(&g, 1.0f, 0.5f, 0, dt);
    CHECK_NEAR(g.pos_setpoint_rad, 1.0f, 1e-6f);
    CHECK_NEAR(g.vel_setpoint_rad_s, 0.5f, 1e-6f);
}

/** @brief 位置P+速度前馈（ODrive：vel_des = vel_ff + pos_gain*pos_err） */
void test_motion_position_p(void) {
    CHECK_NEAR(algo_motion_position(1.0f, 0.5f, 20.0f, 0.3f), 10.3f, 1e-5f);
    CHECK_NEAR(algo_motion_position(1.0f, 0.5f, 0, 0.3f), 0.3f, 1e-6f);
    CHECK_NEAR(algo_motion_position(0.2f, 0.5f, 20.0f, 0), -6.0f, 1e-5f);
}

/** @brief 位置P取模最短路径（单圈绝对模式：±180° 内） */
void test_motion_position_circular_p(void) {
    /* pos_sp=0.1、pos_est=6.2：最短路径误差 = 0.1-6.2+2π ≈ 0.18318 */
    CHECK_NEAR(algo_motion_position(0.1f, 6.2f, 20.0f, 0), 3.66373f, 1e-4f);
    CHECK_NEAR(algo_motion_position(6.2f, 0.1f, 20.0f, 0), -3.66373f, 1e-4f);
    /* 常规小误差不受取模影响 */
    CHECK_NEAR(algo_motion_position(1.0f, 0.5f, 20.0f, 0.3f), 10.3f, 1e-5f);
}

/** @brief 滤波环形：目标跨零点跳变按短路径平滑（不绕大圈） */
void test_motion_pos_filter_circular(void) {
    algo_motion_pos_filter_t f = {6.2f, 0};              /* 设定在 6.2 rad */
    float dt = 2e-4f;
    algo_motion_pos_filter_step(&f, 0.1f, 0, 10.0f, dt); /* 目标跳到 0.1（短路径 +0.183） */
    CHECK(
        f.vel_setpoint_rad_s > 0
        && f.vel_setpoint_rad_s < 0.01f);                /* 正向小速度，不是 -122 的大摆 */
}
