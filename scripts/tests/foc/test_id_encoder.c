/**
 * @file    test_id_encoder.c
 * @brief   id_encoder 用例（仿真转子跟随 → 零点/方向/质量；超时）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

#include "id_encoder.h"

/* 仿真：转子跟随施加电角度。θe_true = p·dir·θm − offset = θapplied
 * → θm = (θapplied + offset) / (p·dir) */
static float sim_rotor_rad(float theta_applied, float offset, float dir, float p) {
    return (theta_applied + offset) / (p * dir);
}

static bool run_identify(id_encoder_t* id, float offset_true, float dir_true, float ts,
                         id_encoder_out_t* out) {
    id_encoder_in_t in;
    int n;

    out->theta_e_cmd = 0.0f;
    for (n = 0; n < 300000; n++) { /* 上限 12s 仿真时间 */
        in.theta_m_raw_rad = foc_wrap_2pi(sim_rotor_rad(out->theta_e_cmd, offset_true, dir_true, 10.0f));
        in.i_d_a = 2.0f;
        in.i_q_a = 0.0f;
        in.v_bus_v = 24.0f;
        in.dt_s = ts;
        id->step(id, &in, out);
        if (out->done || out->failed) {
            return true;
        }
    }
    return false;
}

void test_id_encoder(void) {
    id_encoder_t id;
    id_encoder_cfg_t cfg = {
        .pole_pairs = 10U,
        .i_cal_a = 2.0f,
        .lockin_ms = 100.0f,
        .dir_ms = 50.0f,
        .dir_step_rad = FOC_PI_F / 3.0f,
        .sweep_steps = 36U,
        .sweep_step_ms = 5.0f,
        .quality_min = 0.8f,
        .ratio_tol = 0.2f,
        .timeout_ms = 10000.0f,
    };
    const float ts = 1.0f / 25000.0f;
    const float offset_true = 1.234f;
    id_encoder_out_t out;

    /* 正方向 */
    id_encoder_ctor(&id);
    CHECK(id.init(&id, &cfg) == 0);
    id.reset(&id);
    out = (id_encoder_out_t){0};
    CHECK(run_identify(&id, offset_true, 1.0f, ts, &out));
    CHECK(out.done);
    CHECK(!out.failed);
    CHECK_NEAR(out.direction, 1.0f, 1e-6f);
    CHECK_NEAR(out.offset_rad, offset_true, 0.02f);
    CHECK(out.quality > 0.95f);
    CHECK_NEAR(out.mech_ratio_err, 0.0f, 0.05f);

    /* 反方向 */
    {
        id_encoder_t id2;
        id_encoder_out_t out2;
        id_encoder_ctor(&id2);
        CHECK(id2.init(&id2, &cfg) == 0);
        id2.reset(&id2);
        out2 = (id_encoder_out_t){0};
        CHECK(run_identify(&id2, offset_true, -1.0f, ts, &out2));
        CHECK(out2.done);
        CHECK_NEAR(out2.direction, -1.0f, 1e-6f);
        CHECK_NEAR(out2.offset_rad, offset_true, 0.02f);
    }

    /* 超时：转子不动 → FAILED */
    {
        id_encoder_t id3;
        id_encoder_cfg_t cfg3 = cfg;
        id_encoder_in_t in;
        id_encoder_out_t out3;
        cfg3.timeout_ms = 500.0f;
        id_encoder_ctor(&id3);
        CHECK(id3.init(&id3, &cfg3) == 0);
        id3.reset(&id3);
        out3 = (id_encoder_out_t){0};
        for (int n = 0; n < 100000; n++) {
            in.theta_m_raw_rad = 0.0f;
            in.i_d_a = 2.0f;
            in.i_q_a = 0.0f;
            in.v_bus_v = 24.0f;
            in.dt_s = ts;
            id3.step(&id3, &in, &out3);
            if (out3.done || out3.failed) {
                break;
            }
        }
        CHECK(out3.failed);
    }

    /* 非法配置 */
    {
        id_encoder_t id4;
        id_encoder_cfg_t bad = cfg;
        bad.sweep_steps = 4U;
        id_encoder_ctor(&id4);
        CHECK(id4.init(&id4, &bad) == -1);
    }
}
