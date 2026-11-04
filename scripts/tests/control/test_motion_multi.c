/** @file test_motion_multi.c
 * @brief 多圈连续角、增量命令、行程保护与 MIT 模式的集成测试
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_foc.h"
#include "app_motion.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "foc_math.h"
#include "test_util.h"

extern float g_mock_rotor_rad;
extern void mock_advance_cycle(uint32_t cycles);
extern void mock_param_store_reset(void);
extern void mock_params_reset(void);

/** @brief 标定并置零（reducer=20 / output_ratio=0.95：出轴一圈 = 20 转子圈） */
static void multi_setup(bool enable_traj) {
    mock_param_store_reset();
    mock_params_reset();
    app_motor_params_mutable()->kt_nm_per_a = .117f;
    app_motor_params_mutable()->reducer_ratio = 20.0f;
    app_motor_params_mutable()->encoder.output_ratio = 0.95f;
    app_software_params_mutable()->control.position_loop.pos_gain = 25.0f;
    app_software_params_mutable()->position_traj.trajectory_vel_limit =
        enable_traj ? 30.0f : 0.0f;
    app_software_params_mutable()->position_traj.trajectory_accel_limit =
        enable_traj ? 60.0f : 0.0f;
    g_mock_rotor_rad = 0.0f;
    app_foc_init();
    app_motion_init();
    CHECK(app_motion_gains(2, 50, 0, 0) == 0);
    CHECK(app_motion_calibrate() == 0);
    for (unsigned i = 0; i < 1400; ++i) {
        app_motion_step();
    }
    CHECK(app_motion_apply() == 0);
    app_motion_step();
    CHECK(app_motion_zero() == 0);
}

void test_motion_multi(void) {
    /* ── 1. 轨迹未启用：多圈命令必须拒绝，单圈仍可用 ───────────── */
    multi_setup(false);
    app_motion_snapshot_t s;
    app_motion_get_snapshot(&s);
    CHECK(s.continuous_valid);
    CHECK_NEAR(s.shaft_continuous_rad, 0, 1e-6f);
    CHECK(app_motion_position_multi(0.5f, 0, 0) == APP_MOTION_CMD_TRAJ_DISABLED);

    /* ── 2. 轨迹启用：连续角累积（小步进）─────────────────────── */
    multi_setup(true);
    app_motion_get_snapshot(&s);
    CHECK(s.continuous_valid);
    g_mock_rotor_rad = 0.5f; /* 出轴 +0.025 rad */
    app_motion_step();
    app_motion_get_snapshot(&s);
    CHECK_NEAR(s.shaft_continuous_rad, 0.025f, 0.002f);

    /* ── 3. 跨单圈边界：连续角继续累积，单圈角回绕 ────────────── */
    /* 圈内多步累积（10 步 × 0.25 rad 转子 = 2.5 rad → 出轴 0.125 rad）。
     * 说明：主机 mock 的游标读数与转子同步（无独立双读数），shaft 每转子圈回绕，
     * 因此集成测试只覆盖"圈内累积"；跨 0/2π 的 unwrap 由
     * test_algo_continuous 的纯函数用例覆盖（那里可构造真实边界）。 */
    unsigned rebase_before = s.continuous_rebase_count;
    for (unsigned i = 1; i <= 10; ++i) {
        g_mock_rotor_rad = 0.5f + 0.25f * (float)i;
        app_motion_step();
    }
    app_motion_get_snapshot(&s);
    CHECK_NEAR(s.shaft_continuous_rad, 0.025f + 0.125f, 0.005f);
    CHECK(s.continuous_rebase_count == rebase_before); /* 圈内不应重基准 */
    CHECK(s.continuous_valid);

    /* ── 4. 命令层：绝对多圈 / 增量 / 限幅 ────────────────────── */
    CHECK(app_foc_enable() == 0);
    mock_advance_cycle(48000);
    app_motion_step();
    app_motion_get_snapshot(&s);
    float base = s.shaft_continuous_rad;

    CHECK(app_motion_position_multi(base + 1.0f, 0, 0) == 0);
    CHECK(app_motion_position_rel(0.5f, 0, 0) == 0); /* 同模式可续发 */
    /* 单次位移上限 10 圈 = 62.8 rad */
    CHECK(app_motion_position_multi(base + 100.0f, 0, 0) == APP_MOTION_CMD_TARGET_LIMIT);

    /* 放宽单次位移，单独验证连续角绝对值上限（100 圈 = 628 rad） */
    app_software_params_mutable()->position_travel.max_step_deg = 1.0e6f;
    CHECK(app_motion_position_multi(700.0f, 0, 0) == APP_MOTION_CMD_TARGET_LIMIT);

    /* 可选软限位：启用后越界拒绝 */
    app_software_params_mutable()->position_travel.travel_min_deg = -180.0f;
    app_software_params_mutable()->position_travel.travel_max_deg = 180.0f;
    CHECK(app_motion_position_multi(200.0f * 0.01745329252f, 0, 0)
          == APP_MOTION_CMD_TARGET_LIMIT);
    CHECK(app_motion_position_multi(90.0f * 0.01745329252f, 0, 0) == 0);

    /* 复位软限位与单次位移上限，避免影响后续用例 */
    app_software_params_mutable()->position_travel.travel_min_deg = 0.0f;
    app_software_params_mutable()->position_travel.travel_max_deg = 0.0f;
    app_software_params_mutable()->position_travel.max_step_deg = 3600.0f;
    CHECK(app_motion_stop() == 0);
    app_foc_disable();
    /* 恢复工厂机械参数，避免影响后续用例（本测试为凑步数改过减速比） */
    app_motor_params_mutable()->reducer_ratio = 50.0f;
    app_motor_params_mutable()->encoder.output_ratio = 0.98f;
}

void test_motion_mit(void) {
    multi_setup(true);
    app_software_params_mutable()->mit.enable = 1U;
    app_software_params_mutable()->mit.kp_max = 50.0f;
    app_software_params_mutable()->mit.kd_max = 2.0f;
    app_software_params_mutable()->mit.gain_ramp_rate = 200.0f;
    app_software_params_mutable()->mit.pos_err_max_deg = 90.0f;
    app_software_params_mutable()->mit.t_ff_max_ratio = 1.0f;

    CHECK(app_foc_enable() == 0);
    mock_advance_cycle(48000);
    app_motion_step();

    app_motion_snapshot_t s;
    app_motion_get_snapshot(&s);
    float pos = s.shaft_continuous_rad;

    /* 未启用时拒绝（先用临时关闭验证语义） */
    app_software_params_mutable()->mit.enable = 0U;
    CHECK(app_motion_mit(pos, 0, 10.0f, 1.0f, 0) == APP_MOTION_CMD_MIT_DISABLED);
    app_software_params_mutable()->mit.enable = 1U;

    /* 增益超限拒绝 */
    CHECK(app_motion_mit(pos, 0, 60.0f, 1.0f, 0) == APP_MOTION_CMD_TARGET_LIMIT);
    CHECK(app_motion_mit(pos, 0, 10.0f, 5.0f, 0) == APP_MOTION_CMD_TARGET_LIMIT);
    /* 位置误差入口检查（>90deg） */
    CHECK(app_motion_mit(pos + 3.0f, 0, 10.0f, 1.0f, 0) == APP_MOTION_CMD_TARGET_LIMIT);

    /* 正常进入：增益从 0 软启动 */
    CHECK(app_motion_mit(pos, 0, 10.0f, 1.0f, 0) == 0);
    for (unsigned i = 0; i < 20; ++i) {
        mock_advance_cycle(48000);
        app_motion_step();
    }
    app_motion_get_snapshot(&s);
    CHECK(s.mode == APP_MOTION_MODE_MIT);
    CHECK(s.mit_kp_eff_nm_per_rad > 0.0f);
    CHECK(s.mit_kp_eff_nm_per_rad <= 10.0f);
    CHECK(s.mit_kd_eff_nm_per_rad_s <= 1.0f);
    /* 位置误差 0 → 力矩仅由前馈/速度项构成（此处均为 0） */
    CHECK_NEAR(s.mit_torque_nm, 0.0f, 1e-3f);

    /* 模式互斥：MIT 运行中不得切到位置模式 */
    CHECK(app_motion_position(1.0f, 0, 0) == APP_MOTION_CMD_MODE_LOCKED);

    /* 退出 */
    CHECK(app_motion_stop() == 0);
    app_foc_disable();
    app_motor_params_mutable()->reducer_ratio = 50.0f;
    app_motor_params_mutable()->encoder.output_ratio = 0.98f;
}