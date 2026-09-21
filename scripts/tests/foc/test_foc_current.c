/**
 * @file    test_foc_current.c
 * @brief   foc_current 用例（闭环收敛 / 圆形限幅 / 抗饱和 / 限幅给定 / 异常）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

#include "foc_current.h"

/* 一阶 RL 被控对象：di/dt = (v − R·i)/L */
typedef struct {
    float i_d, i_q;
} rl_plant_t;

static void rl_step(rl_plant_t* p, float vd, float vq, float r, float l, float ts) {
    p->i_d += ts * (vd - r * p->i_d) / l;
    p->i_q += ts * (vq - r * p->i_q) / l;
}

void test_foc_current(void) {
    foc_current_t cur;
    foc_current_cfg_t cfg = {
        .kp = 0.745f,
        .ki = 992.7f,
        .sample_time_s = 1.0f / 25000.0f,
        .decoupling_en = 0U,
        .l_d = 1.185e-4f,
        .l_q = 1.185e-4f,
        .lambda = 0.125f,
        .aw_decay = 0.99f,
    };
    const float r = 0.158f;
    const float l = 1.185e-4f;
    const float ts = cfg.sample_time_s;

    foc_current_ctor(&cur);
    CHECK(cur.init(&cur, &cfg) == 0);

    /* 闭环收敛：i_q 给定 2A，2ms 内误差 < 0.05A */
    {
        rl_plant_t plant = {.i_d = 0.0f, .i_q = 0.0f};
        foc_current_out_t out;
        for (int n = 0; n < 50; n++) {
            foc_current_in_t in = {
                .i_d_ref = 0.0f,
                .i_q_ref = 2.0f,
                .i_d_a = plant.i_d,
                .i_q_a = plant.i_q,
                .v_bus_v = 24.0f,
                .v_max = 12.3f,
                .i_max = 24.3f,
                .omega_e_rad_s = 0.0f,
            };
            CHECK(cur.step(&cur, &in, &out) == 0);
            rl_step(&plant, out.v_d, out.v_q, r, l, ts);
        }
        CHECK_NEAR(plant.i_q, 2.0f, 0.05f);
    }

    /* 圆形电压限幅：输出矢量不超 v_max，且 vd/vq 比例保持 */
    {
        foc_current_t c2;
        foc_current_out_t out;
        foc_current_in_t in = {
            .i_d_ref = 5.0f, .i_q_ref = 5.0f, .i_d_a = 0.0f, .i_q_a = 0.0f,
            .v_bus_v = 24.0f, .v_max = 0.5f, .i_max = 24.3f, .omega_e_rad_s = 0.0f,
        };
        foc_current_ctor(&c2);
        CHECK(c2.init(&c2, &cfg) == 0);
        CHECK(c2.step(&c2, &in, &out) == 0);
        CHECK(out.saturated);
        CHECK(sqrtf(out.v_d * out.v_d + out.v_q * out.v_q) <= 0.5f + 1e-5f);
        CHECK_NEAR(out.v_d / out.v_q, 1.0f, 1e-4f);
    }

    /* 抗饱和：持续饱和后给定回零，积分未发散 */
    {
        foc_current_t c3;
        foc_current_out_t out;
        foc_current_in_t in = {
            .i_d_ref = 10.0f, .i_q_ref = 10.0f, .i_d_a = 0.0f, .i_q_a = 0.0f,
            .v_bus_v = 24.0f, .v_max = 0.5f, .i_max = 24.3f, .omega_e_rad_s = 0.0f,
        };
        foc_current_ctor(&c3);
        CHECK(c3.init(&c3, &cfg) == 0);
        for (int n = 0; n < 1000; n++) {
            (void)c3.step(&c3, &in, &out);
        }
        in.i_d_ref = 0.0f;
        in.i_q_ref = 0.0f;
        (void)c3.step(&c3, &in, &out);
        CHECK(sqrtf(out.v_d * out.v_d + out.v_q * out.v_q) < 0.1f);
    }

    /* 电流矢量限幅（圆形，保角） */
    {
        foc_current_t c4;
        foc_current_out_t out;
        foc_current_in_t in = {
            .i_d_ref = 30.0f, .i_q_ref = 40.0f, .i_d_a = 0.0f, .i_q_a = 0.0f,
            .v_bus_v = 24.0f, .v_max = 12.3f, .i_max = 10.0f, .omega_e_rad_s = 0.0f,
        };
        foc_current_ctor(&c4);
        CHECK(c4.init(&c4, &cfg) == 0);
        CHECK(c4.step(&c4, &in, &out) == 0);
        CHECK_NEAR(sqrtf(out.i_d_ref_lim * out.i_d_ref_lim + out.i_q_ref_lim * out.i_q_ref_lim),
                   10.0f, 1e-4f);
        CHECK_NEAR(out.i_d_ref_lim / out.i_q_ref_lim, 30.0f / 40.0f, 1e-4f);
    }

    /* 非有限反馈 → 给定归零 */
    {
        foc_current_t c5;
        foc_current_out_t out;
        foc_current_in_t in = {
            .i_d_ref = 1.0f, .i_q_ref = 1.0f, .i_d_a = 0.0f / 0.0f, .i_q_a = 0.0f,
            .v_bus_v = 24.0f, .v_max = 12.3f, .i_max = 24.3f, .omega_e_rad_s = 0.0f,
        };
        foc_current_ctor(&c5);
        CHECK(c5.init(&c5, &cfg) == 0);
        CHECK(c5.step(&c5, &in, &out) == 0);
        CHECK_NEAR(out.i_d_ref_lim, 0.0f, 1e-6f);
        CHECK_NEAR(out.i_q_ref_lim, 0.0f, 1e-6f);
    }

    /* set_gains 运行中更新：kp=1、ki=0 → v_q = e */
    {
        foc_current_t c6;
        foc_current_out_t out;
        foc_current_in_t in = {
            .i_d_ref = 0.0f, .i_q_ref = 1.0f, .i_d_a = 0.0f, .i_q_a = 0.0f,
            .v_bus_v = 24.0f, .v_max = 12.3f, .i_max = 24.3f, .omega_e_rad_s = 0.0f,
        };
        foc_current_ctor(&c6);
        CHECK(c6.init(&c6, &cfg) == 0);
        c6.set_gains(&c6, 1.0f, 0.0f);
        CHECK(c6.step(&c6, &in, &out) == 0);
        CHECK_NEAR(out.v_q, 1.0f, 1e-5f);
    }

    /* 非法配置 */
    {
        foc_current_t c7;
        foc_current_cfg_t bad = cfg;
        bad.aw_decay = 0.0f;
        foc_current_ctor(&c7);
        CHECK(c7.init(&c7, &bad) == -1);
    }
}
