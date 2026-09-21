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

    /* 非有限反馈 → 给定归零 + 输出零电压（有限） */
    {
        foc_current_t c5;
        foc_current_out_t out;
        foc_current_in_t in = {
            .i_d_ref = 1.0f, .i_q_ref = 1.0f, .i_d_a = NAN, .i_q_a = 0.0f,
            .v_bus_v = 24.0f, .v_max = 12.3f, .i_max = 24.3f, .omega_e_rad_s = 0.0f,
        };
        foc_current_ctor(&c5);
        CHECK(c5.init(&c5, &cfg) == 0);
        CHECK(c5.step(&c5, &in, &out) == 0);
        CHECK_NEAR(out.i_d_ref_lim, 0.0f, 1e-6f);
        CHECK_NEAR(out.i_q_ref_lim, 0.0f, 1e-6f);
        CHECK(foc_finite(out.v_d) && foc_finite(out.v_q));
        CHECK_NEAR(out.v_d, 0.0f, 1e-6f);
        CHECK_NEAR(out.v_q, 0.0f, 1e-6f);

        /* +Inf 反馈 */
        in.i_d_a = INFINITY;
        in.i_q_a = -INFINITY;
        CHECK(c5.step(&c5, &in, &out) == 0);
        CHECK(foc_finite(out.v_d) && foc_finite(out.v_q));
        CHECK_NEAR(out.v_d, 0.0f, 1e-6f);
        CHECK_NEAR(out.v_q, 0.0f, 1e-6f);
    }

    /* 解耦前馈开启：精确校验 vd_ff/vq_ff（对照关闭时） */
    {
        foc_current_t c8;
        foc_current_cfg_t c8cfg = cfg;
        foc_current_out_t out_on, out_off;
        foc_current_in_t in = {
            .i_d_ref = 0.0f, .i_q_ref = 0.0f,
            .i_d_a = 2.0f, .i_q_a = 3.0f,
            .v_bus_v = 24.0f, .v_max = 12.3f, .i_max = 24.3f,
            .omega_e_rad_s = 100.0f,
        };
        c8cfg.decoupling_en = 1U;
        c8cfg.l_d = 1.0e-4f;
        c8cfg.l_q = 1.0e-4f;
        c8cfg.lambda = 0.1f;
        foc_current_ctor(&c8);
        CHECK(c8.init(&c8, &c8cfg) == 0);
        CHECK(c8.step(&c8, &in, &out_on) == 0);
        /* vd = kp·(−2) − 100·1e-4·3 = −1.49 − 0.03 = −1.52
         * vq = kp·(−3) + 100·(1e-4·2 + 0.1) = −2.235 + 10.02 = 7.785 */
        CHECK_NEAR(out_on.v_d, -1.52f, 1e-3f);
        CHECK_NEAR(out_on.v_q, 7.785f, 1e-2f);

        c8cfg.decoupling_en = 0U;
        foc_current_ctor(&c8);
        CHECK(c8.init(&c8, &c8cfg) == 0);
        CHECK(c8.step(&c8, &in, &out_off) == 0);
        CHECK_NEAR(out_off.v_d, -1.49f, 1e-3f);
        CHECK_NEAR(out_off.v_q, -2.235f, 1e-3f);
    }

    /* set_decoupling：运行中开启后与 init 开启等效（对照关闭值） */
    {
        foc_current_t c13;
        foc_current_cfg_t c13cfg = cfg;
        foc_current_out_t out;
        foc_current_in_t in = {
            .i_d_ref = 0.0f, .i_q_ref = 0.0f,
            .i_d_a = 2.0f, .i_q_a = 3.0f,
            .v_bus_v = 24.0f, .v_max = 12.3f, .i_max = 24.3f,
            .omega_e_rad_s = 100.0f,
        };
        c13cfg.decoupling_en = 0U;
        c13cfg.l_d = 1.0e-4f;
        c13cfg.l_q = 1.0e-4f;
        c13cfg.lambda = 0.1f;
        foc_current_ctor(&c13);
        CHECK(c13.init(&c13, &c13cfg) == 0);
        CHECK(c13.step(&c13, &in, &out) == 0);
        CHECK_NEAR(out.v_q, -2.235f, 1e-3f); /* 前馈关闭 */
        c13.reset(&c13); /* 隔离积分器，仅比较前馈效果 */
        c13.set_decoupling(&c13, 1U);
        CHECK(c13.step(&c13, &in, &out) == 0);
        CHECK_NEAR(out.v_q, 7.785f, 1e-2f); /* 前馈开启（与 init 开启等效） */
        c13.reset(&c13);
        c13.set_decoupling(&c13, 0U);
        CHECK(c13.step(&c13, &in, &out) == 0);
        CHECK_NEAR(out.v_q, -2.235f, 1e-3f); /* 再次关闭 */
    }

    /* 前馈开启 + 非有限 ωe → 按 0 处理，输出仍有限 */
    {
        foc_current_t c9;
        foc_current_cfg_t c9cfg = cfg;
        foc_current_out_t out;
        foc_current_in_t in = {
            .i_d_ref = 0.0f, .i_q_ref = 0.0f,
            .i_d_a = 1.0f, .i_q_a = 1.0f,
            .v_bus_v = 24.0f, .v_max = 12.3f, .i_max = 24.3f,
            .omega_e_rad_s = NAN,
        };
        c9cfg.decoupling_en = 1U;
        foc_current_ctor(&c9);
        CHECK(c9.init(&c9, &c9cfg) == 0);
        CHECK(c9.step(&c9, &in, &out) == 0);
        CHECK(foc_finite(out.v_d) && foc_finite(out.v_q));
    }

    /* reset：饱和后复位，积分清零 */
    {
        foc_current_t c10;
        foc_current_out_t out;
        foc_current_in_t in = {
            .i_d_ref = 10.0f, .i_q_ref = 0.0f, .i_d_a = 0.0f, .i_q_a = 0.0f,
            .v_bus_v = 24.0f, .v_max = 0.5f, .i_max = 24.3f, .omega_e_rad_s = 0.0f,
        };
        foc_current_ctor(&c10);
        CHECK(c10.init(&c10, &cfg) == 0);
        for (int n = 0; n < 200; n++) {
            (void)c10.step(&c10, &in, &out);
        }
        CHECK(fabsf(out.v_d) > 0.1f); /* 有积分贡献 */
        c10.reset(&c10);
        in.i_d_ref = 0.0f;
        in.i_q_ref = 0.0f;
        (void)c10.step(&c10, &in, &out);
        CHECK_NEAR(out.v_d, 0.0f, 1e-6f);
        CHECK_NEAR(out.v_q, 0.0f, 1e-6f);
    }

    /* 退化限幅：v_max = 0 → 零输出且饱和；i_max = 0 → 给定归零 */
    {
        foc_current_t c11;
        foc_current_out_t out;
        foc_current_in_t in = {
            .i_d_ref = 1.0f, .i_q_ref = 1.0f, .i_d_a = 0.0f, .i_q_a = 0.0f,
            .v_bus_v = 24.0f, .v_max = 0.0f, .i_max = 24.3f, .omega_e_rad_s = 0.0f,
        };
        foc_current_ctor(&c11);
        CHECK(c11.init(&c11, &cfg) == 0);
        CHECK(c11.step(&c11, &in, &out) == 0);
        CHECK(out.saturated);
        CHECK_NEAR(out.v_d, 0.0f, 1e-6f);
        CHECK_NEAR(out.v_q, 0.0f, 1e-6f);

        in.v_max = 12.3f;
        in.i_max = 0.0f;
        CHECK(c11.step(&c11, &in, &out) == 0);
        CHECK_NEAR(out.i_d_ref_lim, 0.0f, 1e-6f);
        CHECK_NEAR(out.i_q_ref_lim, 0.0f, 1e-6f);
    }

    /* 未初始化对象 → step 返回 -1 */
    {
        foc_current_t c12;
        foc_current_out_t out;
        foc_current_in_t in = {
            .i_d_ref = 0.0f, .i_q_ref = 0.0f, .i_d_a = 0.0f, .i_q_a = 0.0f,
            .v_bus_v = 24.0f, .v_max = 12.3f, .i_max = 24.3f, .omega_e_rad_s = 0.0f,
        };
        foc_current_ctor(&c12);
        CHECK(c12.step(&c12, &in, &out) == -1);
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

    /* 非法配置：aw_decay 越界 / 非有限增益与时间常数 */
    {
        foc_current_t c7;
        foc_current_cfg_t bad = cfg;
        foc_current_ctor(&c7);
        bad.aw_decay = 0.0f;
        CHECK(c7.init(&c7, &bad) == -1);
        bad = cfg;
        bad.kp = NAN;
        CHECK(c7.init(&c7, &bad) == -1);
        bad = cfg;
        bad.ki = NAN;
        CHECK(c7.init(&c7, &bad) == -1);
        bad = cfg;
        bad.sample_time_s = NAN;
        CHECK(c7.init(&c7, &bad) == -1);
        bad = cfg;
        bad.lambda = INFINITY;
        CHECK(c7.init(&c7, &bad) == -1);
    }
}
