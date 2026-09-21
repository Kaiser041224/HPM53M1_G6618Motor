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
        .sweep_settle_ms = 100.0f,
        .sweep_turns = 1.0f,
        .hyst_max_rad = 0.0f, /* 用例单独启用回差检查 */
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
    CHECK_NEAR(out.theta_e_cmd, 0.0f, 0.001f); /* REV 终点 = 0：验证阶段以 0 为参考 */
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

    /* 非法配置（含非有限字段） */
    {
        id_encoder_t id4;
        id_encoder_cfg_t bad = cfg;
        id_encoder_ctor(&id4);
        bad.sweep_steps = 4U;
        CHECK(id4.init(&id4, &bad) == -1);
        bad = cfg;
        bad.timeout_ms = NAN;
        CHECK(id4.init(&id4, &bad) == -1);
        bad = cfg;
        bad.dir_step_rad = 0.0f;
        CHECK(id4.init(&id4, &bad) == -1);
        bad = cfg;
        bad.quality_min = 1.5f;
        CHECK(id4.init(&id4, &bad) == -1);
        bad = cfg;
        bad.ratio_tol = -0.1f;
        CHECK(id4.init(&id4, &bad) == -1);
    }

    /* 方向判定无效：转子始终不动 → FAILED(DIR) */
    {
        id_encoder_t id5;
        id_encoder_in_t in;
        id_encoder_out_t out5;
        id_encoder_ctor(&id5);
        CHECK(id5.init(&id5, &cfg) == 0);
        id5.reset(&id5);
        out5 = (id_encoder_out_t){0};
        out5.progress = 777.0f; /* 验证超时/失败路径会写 progress */
        for (int n = 0; n < 100000; n++) {
            in.theta_m_raw_rad = 0.0f;
            in.i_d_a = 2.0f;
            in.i_q_a = 0.0f;
            in.v_bus_v = 24.0f;
            in.dt_s = ts;
            id5.step(&id5, &in, &out5);
            if (out5.done || out5.failed) {
                break;
            }
        }
        CHECK(out5.failed);
        CHECK(out5.fail_reason == ID_ENCODER_FAIL_DIR);
        CHECK(out5.progress != 777.0f);
        CHECK_NEAR(out5.i_d_ref, 0.0f, 1e-6f); /* 终止态零给定 */
    }

    /* 非有限测量样本 > 3 → FAILED(NONFINITE) */
    {
        id_encoder_t id6;
        id_encoder_in_t in;
        id_encoder_out_t out6;
        id_encoder_ctor(&id6);
        CHECK(id6.init(&id6, &cfg) == 0);
        id6.reset(&id6);
        out6 = (id_encoder_out_t){0};
        for (int n = 0; n < 10; n++) {
            in.theta_m_raw_rad = NAN;
            in.i_d_a = 2.0f;
            in.i_q_a = 0.0f;
            in.v_bus_v = 24.0f;
            in.dt_s = ts;
            id6.step(&id6, &in, &out6);
            if (out6.done || out6.failed) {
                break;
            }
        }
        CHECK(out6.failed);
        CHECK(out6.fail_reason == ID_ENCODER_FAIL_NONFINITE);
        CHECK_NEAR(out6.i_d_ref, 0.0f, 1e-6f);
    }

    /* 质量不足：转子在扫描段不跟随 → FAILED(QUALITY) */
    {
        id_encoder_t id7;
        id_encoder_in_t in;
        id_encoder_out_t out7;
        float hold_theta_m = 0.0f;
        bool sweep_started = false;
        id_encoder_ctor(&id7);
        CHECK(id7.init(&id7, &cfg) == 0);
        id7.reset(&id7);
        out7 = (id_encoder_out_t){0};
        for (int n = 0; n < 300000; n++) {
            float theta_m;
            if (out7.phase >= ID_ENCODER_PHASE_SWEEP_FWD) {
                if (!sweep_started) {
                    hold_theta_m = foc_wrap_2pi((out7.theta_e_cmd + 1.234f) / 10.0f);
                    sweep_started = true;
                }
                theta_m = hold_theta_m; /* 转子卡住不跟随 */
            } else {
                theta_m = foc_wrap_2pi((out7.theta_e_cmd + 1.234f) / 10.0f);
            }
            in.theta_m_raw_rad = theta_m;
            in.i_d_a = 2.0f;
            in.i_q_a = 0.0f;
            in.v_bus_v = 24.0f;
            in.dt_s = ts;
            id7.step(&id7, &in, &out7);
            if (out7.done || out7.failed) {
                break;
            }
        }
        CHECK(out7.failed);
        CHECK(out7.fail_reason == ID_ENCODER_FAIL_QUALITY);
    }

    /* 多圈扫描（turns=3）：零点/质量/行程校验仍成立 */
    {
        id_encoder_t id12;
        id_encoder_in_t in;
        id_encoder_out_t out12;
        id_encoder_cfg_t c12 = cfg;

        c12.sweep_turns = 3.0f;
        id_encoder_ctor(&id12);
        CHECK(id12.init(&id12, &c12) == 0);
        id12.reset(&id12);
        out12 = (id_encoder_out_t){0};
        for (int n = 0; n < 300000; n++) {
            float theta_m = foc_wrap_2pi((out12.theta_e_cmd + 1.234f) / 10.0f);

            in.theta_m_raw_rad = theta_m;
            in.i_d_a = 2.0f;
            in.i_q_a = 0.0f;
            in.v_bus_v = 24.0f;
            in.dt_s = ts;
            id12.step(&id12, &in, &out12);
            if (out12.done || out12.failed) {
                break;
            }
        }
        CHECK(out12.done);
        CHECK_NEAR(out12.offset_rad, 1.234f, 0.05f);
        CHECK(out12.quality > 0.95f);
        CHECK_NEAR(out12.mech_ratio_err, 0.0f, 0.05f);
    }

    /* 传动打滑：反向扫描零点偏移 0.6 rad（34°）→ FAILED(HYST) */
    {
        id_encoder_t id13;
        id_encoder_in_t in;
        id_encoder_out_t out13;
        id_encoder_cfg_t c13 = cfg;
        c13.hyst_max_rad = 0.5f;
        id_encoder_ctor(&id13);
        CHECK(id13.init(&id13, &c13) == 0);
        id13.reset(&id13);
        out13 = (id_encoder_out_t){0};
        for (int n = 0; n < 300000; n++) {
            /* 反向扫描段用偏移 0.6 rad 的传动：模拟联轴打滑（按相位判定，避免一拍滞后） */
            float off =
                (out13.phase == ID_ENCODER_PHASE_SWEEP_REV) ? 1.834f : 1.234f;
            float theta_m = foc_wrap_2pi((out13.theta_e_cmd + off) / 10.0f);

            in.theta_m_raw_rad = theta_m;
            in.i_d_a = 2.0f;
            in.i_q_a = 0.0f;
            in.v_bus_v = 24.0f;
            in.dt_s = ts;
            id13.step(&id13, &in, &out13);
            if (out13.done || out13.failed) {
                break;
            }
        }
        CHECK(out13.failed);
        CHECK(out13.fail_reason == ID_ENCODER_FAIL_HYST);
    }

    /* 等效极对数不符（真 9 对极 vs 配置 10）→ ratio_err ≈ +11.1%（诊断判据固化） */
    {
        id_encoder_t id11;
        id_encoder_in_t in;
        id_encoder_out_t out11;
        id_encoder_ctor(&id11);
        CHECK(id11.init(&id11, &cfg) == 0);
        id11.reset(&id11);
        out11 = (id_encoder_out_t){0};
        for (int n = 0; n < 300000; n++) {
            /* 真机 9 对极：θm = (θapplied + offset) / 9 */
            float theta_m = foc_wrap_2pi((out11.theta_e_cmd + 1.234f) / 9.0f);

            in.theta_m_raw_rad = theta_m;
            in.i_d_a = 2.0f;
            in.i_q_a = 0.0f;
            in.v_bus_v = 24.0f;
            in.dt_s = ts;
            id11.step(&id11, &in, &out11);
            if (out11.done || out11.failed) {
                break;
            }
        }
        CHECK(out11.done || out11.failed);
        CHECK_NEAR(out11.mech_ratio_err, 0.1111f, 0.015f); /* 10/9 − 1 */
    }

    /* 总超时：正常跟随但超时极短 → FAILED(TIMEOUT) */
    {
        id_encoder_t id9;
        id_encoder_cfg_t cfg9 = cfg;
        id_encoder_in_t in;
        id_encoder_out_t out9;
        cfg9.lockin_ms = 10.0f;
        cfg9.dir_ms = 10.0f;
        cfg9.timeout_ms = 50.0f; /* 扫描需 180×5ms，必然超时 */
        id_encoder_ctor(&id9);
        CHECK(id9.init(&id9, &cfg9) == 0);
        id9.reset(&id9);
        out9 = (id_encoder_out_t){0};
        for (int n = 0; n < 300000; n++) {
            in.theta_m_raw_rad = foc_wrap_2pi((out9.theta_e_cmd + 1.234f) / 10.0f);
            in.i_d_a = 2.0f;
            in.i_q_a = 0.0f;
            in.v_bus_v = 24.0f;
            in.dt_s = ts;
            id9.step(&id9, &in, &out9);
            if (out9.done || out9.failed) {
                break;
            }
        }
        CHECK(out9.failed);
        CHECK(out9.fail_reason == ID_ENCODER_FAIL_TIMEOUT);
        CHECK_NEAR(out9.i_d_ref, 0.0f, 1e-6f);
    }

    /* 未初始化调用：安全默认（零给定 + FAILED(CONFIG)） */
    {
        id_encoder_t id10;
        id_encoder_in_t in = {
            .theta_m_raw_rad = 0.0f,
            .i_d_a = 0.0f,
            .i_q_a = 0.0f,
            .v_bus_v = 24.0f,
            .dt_s = 1.0f / 25000.0f,
        };
        id_encoder_out_t out10;
        id_encoder_ctor(&id10);
        out10 = (id_encoder_out_t){0};
        id10.step(&id10, &in, &out10);
        CHECK(out10.failed);
        CHECK(out10.fail_reason == ID_ENCODER_FAIL_CONFIG);
        CHECK_NEAR(out10.i_d_ref, 0.0f, 1e-6f);
    }

    /* 极对数校验失败：扫描后 30% 转子打滑（行程不足）→ FAILED(RATIO) */
    {
        id_encoder_t id8;
        id_encoder_cfg_t cfg8 = cfg;
        id_encoder_in_t in;
        id_encoder_out_t out8;
        float hold_theta_m = 0.0f;
        bool slipped = false;
        cfg8.quality_min = 0.3f; /* 放低质量门限以隔离 RATIO 判定 */
        id_encoder_ctor(&id8);
        CHECK(id8.init(&id8, &cfg8) == 0);
        id8.reset(&id8);
        out8 = (id_encoder_out_t){0};
        for (int n = 0; n < 300000; n++) {
            float theta_m;
            bool in_sweep = (out8.phase == ID_ENCODER_PHASE_SWEEP_FWD);
            bool late_sweep = in_sweep && (out8.progress > 0.35f);

            if (late_sweep) {
                if (!slipped) {
                    hold_theta_m = foc_wrap_2pi((out8.theta_e_cmd + 1.234f) / 10.0f);
                    slipped = true;
                }
                theta_m = hold_theta_m; /* 打滑：停止跟随 */
            } else {
                theta_m = foc_wrap_2pi((out8.theta_e_cmd + 1.234f) / 10.0f);
            }
            in.theta_m_raw_rad = theta_m;
            in.i_d_a = 2.0f;
            in.i_q_a = 0.0f;
            in.v_bus_v = 24.0f;
            in.dt_s = ts;
            id8.step(&id8, &in, &out8);
            if (out8.done || out8.failed) {
                break;
            }
        }
        CHECK(out8.failed);
        CHECK(out8.fail_reason == ID_ENCODER_FAIL_RATIO);
    }
}
