/** @file test_algo_mit.c
 * @brief MIT 阻抗控制纯算法测试（公式/斜坡/限幅/拒绝语义）
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "algo_mit.h"
#include "test_util.h"

static algo_mit_cfg_t cfg_default(void) {
    algo_mit_cfg_t cfg = {
        .kp_max_nm_per_rad = 50.0f,
        .kd_max_nm_per_rad_s = 2.0f,
        .gain_ramp_rate = 0.0f, /* 直通 */
        .pos_err_max_rad = 0.5f,
        .t_ff_max_ratio = 1.0f,
    };
    return cfg;
}

void test_algo_mit(void) {
    {
        algo_mit_cfg_t cfg = cfg_default();
        algo_mit_state_t st;
        algo_mit_cmd_t cmd = {
            .p_des_rad = 0.1f, .v_des_rad_s = 0.0f, .kp_nm_per_rad = 10.0f,
            .kd_nm_per_rad_s = 0.0f, .t_ff_nm = 0.0f};

        algo_mit_reset(&st);
        CHECK(algo_mit_step(&cfg, &st, &cmd, 0.0f, 0.0f, 10.0f, 1.0f / 15000.0f) == ALGO_MIT_OK);
        CHECK_NEAR(st.torque_nm, 1.0f, 1e-6f);
        CHECK(!st.saturated);
    }
    {
        /* 阻尼项 */
        algo_mit_cfg_t cfg = cfg_default();
        algo_mit_state_t st;
        algo_mit_cmd_t cmd = {
            .p_des_rad = 0.0f, .v_des_rad_s = 0.5f, .kp_nm_per_rad = 0.0f,
            .kd_nm_per_rad_s = 2.0f, .t_ff_nm = 0.0f};

        algo_mit_reset(&st);
        CHECK(algo_mit_step(&cfg, &st, &cmd, 0.0f, 0.0f, 10.0f, 1.0f / 15000.0f) == ALGO_MIT_OK);
        CHECK_NEAR(st.torque_nm, 1.0f, 1e-6f);
    }
    {
        /* 前馈 + 力矩限幅 */
        algo_mit_cfg_t cfg = cfg_default();
        algo_mit_state_t st;
        algo_mit_cmd_t cmd = {
            .p_des_rad = 0.0f, .v_des_rad_s = 0.0f, .kp_nm_per_rad = 0.0f,
            .kd_nm_per_rad_s = 0.0f, .t_ff_nm = 5.0f};

        algo_mit_reset(&st);
        CHECK(algo_mit_step(&cfg, &st, &cmd, 0.0f, 0.0f, 1.0f, 1.0f / 15000.0f) == ALGO_MIT_OK);
        CHECK_NEAR(st.torque_nm, 1.0f, 1e-6f);
        CHECK(st.saturated);
    }
    {
        /* 前馈占比限幅：ratio=0.5, limit=2 → |ff| ≤ 1 */
        algo_mit_cfg_t cfg = cfg_default();
        algo_mit_state_t st;
        algo_mit_cmd_t cmd = {
            .p_des_rad = 0.0f, .v_des_rad_s = 0.0f, .kp_nm_per_rad = 0.0f,
            .kd_nm_per_rad_s = 0.0f, .t_ff_nm = 5.0f};

        cfg.t_ff_max_ratio = 0.5f;
        algo_mit_reset(&st);
        CHECK(algo_mit_step(&cfg, &st, &cmd, 0.0f, 0.0f, 2.0f, 1.0f / 15000.0f) == ALGO_MIT_OK);
        CHECK_NEAR(st.torque_nm, 1.0f, 1e-6f);
        CHECK(!st.saturated);
    }
    {
        /* 增益斜坡：从 0 起，每拍按 rate*dt 逼近 */
        algo_mit_cfg_t cfg = cfg_default();
        algo_mit_state_t st;
        algo_mit_cmd_t cmd = {
            .p_des_rad = 0.1f, .v_des_rad_s = 0.0f, .kp_nm_per_rad = 10.0f,
            .kd_nm_per_rad_s = 0.0f, .t_ff_nm = 0.0f};

        cfg.gain_ramp_rate = 1000.0f; /* 1000*0.001 = 1 Nm/rad 每拍 */
        algo_mit_reset(&st);
        CHECK(algo_mit_step(&cfg, &st, &cmd, 0.0f, 0.0f, 10.0f, 1.0f / 1000.0f) == ALGO_MIT_OK);
        CHECK_NEAR(st.kp_eff_nm_per_rad, 1.0f, 1e-6f);
        for (int i = 0; i < 20; ++i) {
            (void)algo_mit_step(&cfg, &st, &cmd, 0.0f, 0.0f, 10.0f, 1.0f / 1000.0f);
        }
        CHECK_NEAR(st.kp_eff_nm_per_rad, 10.0f, 1e-5f);
    }
    {
        /* 增益超上限：拒绝，不更新力矩 */
        algo_mit_cfg_t cfg = cfg_default();
        algo_mit_state_t st;
        algo_mit_cmd_t cmd = {
            .p_des_rad = 0.1f, .v_des_rad_s = 0.0f, .kp_nm_per_rad = 60.0f,
            .kd_nm_per_rad_s = 0.0f, .t_ff_nm = 0.0f};

        algo_mit_reset(&st);
        st.torque_nm = 123.0f;
        CHECK(algo_mit_step(&cfg, &st, &cmd, 0.0f, 0.0f, 10.0f, 1.0f / 15000.0f)
              == ALGO_MIT_GAIN_LIMIT);
        CHECK(st.gain_limited);
        CHECK_NEAR(st.torque_nm, 123.0f, 0.0f); /* 出错不改写输出 */
    }
    {
        /* 负增益/非有限/非法 limit 一律拒绝 */
        algo_mit_cfg_t cfg = cfg_default();
        algo_mit_state_t st;
        algo_mit_cmd_t cmd = {
            .p_des_rad = 0.0f, .v_des_rad_s = 0.0f, .kp_nm_per_rad = -1.0f,
            .kd_nm_per_rad_s = 0.0f, .t_ff_nm = 0.0f};

        algo_mit_reset(&st);
        CHECK(algo_mit_step(&cfg, &st, &cmd, 0.0f, 0.0f, 10.0f, 1.0f / 15000.0f)
              == ALGO_MIT_BAD_PARAM);
        cmd.kp_nm_per_rad = 0.0f;
        CHECK(algo_mit_step(&cfg, &st, &cmd, 0.0f, 0.0f, 0.0f, 1.0f / 15000.0f)
              == ALGO_MIT_BAD_PARAM);
        CHECK(algo_mit_step(&cfg, &st, &cmd, 0.0f, 0.0f, 10.0f, 0.0f) == ALGO_MIT_BAD_PARAM);
        CHECK(algo_mit_step(NULL, &st, &cmd, 0.0f, 0.0f, 10.0f, 1.0f / 15000.0f)
              == ALGO_MIT_BAD_PARAM);
    }
    {
        /* 位置误差钳制：不拒绝输出，但误差与力矩按上限截断 */
        algo_mit_cfg_t cfg = cfg_default();
        algo_mit_state_t st;
        algo_mit_cmd_t cmd = {
            .p_des_rad = 1.0f, .v_des_rad_s = 0.0f, .kp_nm_per_rad = 10.0f,
            .kd_nm_per_rad_s = 0.0f, .t_ff_nm = 0.0f};

        cfg.pos_err_max_rad = 0.2f;
        algo_mit_reset(&st);
        CHECK(algo_mit_step(&cfg, &st, &cmd, 0.0f, 0.0f, 100.0f, 1.0f / 15000.0f) == ALGO_MIT_OK);
        CHECK(st.err_clamped);
        CHECK_NEAR(st.pos_err_rad, 0.2f, 1e-6f);
        CHECK_NEAR(st.torque_nm, 2.0f, 1e-6f);
    }
    {
        /* Kd 斜坡应与 Kp 使用同一时间常数（kd_max/kp_max 比例） */
        algo_mit_cfg_t cfg = cfg_default();
        algo_mit_state_t st;
        algo_mit_cmd_t cmd = {
            .p_des_rad = 0.0f, .v_des_rad_s = 0.0f, .kp_nm_per_rad = 0.0f,
            .kd_nm_per_rad_s = 2.0f, .t_ff_nm = 0.0f};

        cfg.gain_ramp_rate = 1000.0f; /* kd 速率 = 1000 * (2/50) = 40/s */
        algo_mit_reset(&st);
        CHECK(algo_mit_step(&cfg, &st, &cmd, 0.0f, 0.0f, 10.0f, 1.0f / 1000.0f) == ALGO_MIT_OK);
        CHECK_NEAR(st.kd_eff_nm_per_rad_s, 0.04f, 1e-6f);
    }
}