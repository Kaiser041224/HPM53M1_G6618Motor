/**
 * @file    test_inertia_identify.c
 * @brief   cal inertia 启动/中止守卫测试（不推进真实双窗口）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_foc.h"
#include "app_inertia_identify.h"
#include "app_motion.h"
#include "app_motor_params.h"
#include "test_util.h"

extern void mock_params_reset(void);

void test_inertia_identify_start(void) {
    mock_params_reset();
    app_motor_params_mutable()->kt_nm_per_a = .117f;
    app_motor_params_mutable()->reducer_ratio = 50.0f;
    app_motor_params_mutable()->inertia_kgm2 = 2.3e-5f;
    app_motor_params_mutable()->load_inertia_kgm2 = 0.0f;
    app_foc_init();
    app_motion_init();

    /* FOC OFF：拒绝，不得占用电流源 */
    CHECK(app_inertia_identify_start(0.5f) == -2);

    CHECK(app_foc_enable() == 0);
    CHECK(app_inertia_identify_start(0.5f) == 0);
    app_inertia_identify_snapshot_t snap;
    app_inertia_identify_get_snapshot(&snap);
    CHECK(snap.busy);
    CHECK(snap.state == APP_INERTIA_IDENTIFY_SETTLE);
    CHECK_NEAR(snap.current_a, 0.5f, 1e-6f);
    CHECK_NEAR(snap.torque_nm, 50.0f * .117f * 0.5f, 1e-5f);

    app_inertia_identify_abort();
    app_inertia_identify_get_snapshot(&snap);
    CHECK(!snap.busy);
    CHECK(snap.state == APP_INERTIA_IDENTIFY_IDLE);
    CHECK(snap.failure == APP_INERTIA_IDENTIFY_FAIL_ABORTED);

    app_foc_disable();
}

/** @brief 模拟单惯量+库仑摩擦：验证双向窗口不再在负转矩段过零 */
static float s_plant_speed;
static void inertia_plant_step(float torque_nm, float dt_s) {
    const float friction_nm = 1.5f;
    const float j_total = 0.115f;
    float net = torque_nm;

    if (s_plant_speed > 0.01f) {
        net -= friction_nm;
    } else if (s_plant_speed < -0.01f) {
        net += friction_nm;
    } else if (fabsf(torque_nm) <= friction_nm) {
        net = 0.0f;
    } else {
        net = torque_nm - copysignf(friction_nm, torque_nm);
    }
    s_plant_speed += (net / j_total) * dt_s;
    if (s_plant_speed > -1.0e-6f && s_plant_speed < 1.0e-6f) {
        s_plant_speed = 0.0f;
    }
    extern float g_mock_rotor_rad;
    g_mock_rotor_rad += s_plant_speed * 50.0f * dt_s;
    while (g_mock_rotor_rad >= 6.283185307f) {
        g_mock_rotor_rad -= 6.283185307f;
    }
}

void test_inertia_identify_simulation(void) {
    extern float g_mock_rotor_rad;
    extern void mock_advance_cycle(uint32_t cycles);

    mock_params_reset();
    app_motor_params_mutable()->kt_nm_per_a = .117f;
    app_motor_params_mutable()->reducer_ratio = 50.0f;
    app_motor_params_mutable()->inertia_kgm2 = 2.3e-5f;
    app_motor_params_mutable()->load_inertia_kgm2 = 0.0f;
    app_foc_init();
    app_motion_init();
    app_inertia_identify_init();
    CHECK(app_foc_enable() == 0);
    CHECK(app_inertia_identify_start(2.0f) == 0);
    s_plant_speed = 0.0f;
    g_mock_rotor_rad = 0.0f;

    for (uint32_t ms = 0U; (ms < 3000U) && app_inertia_identify_busy(); ms++) {
        for (uint32_t j = 0U; j < 30U; j++) {
            float torque = 50.0f * .117f * app_foc_get_iq_ref();
            inertia_plant_step(torque, 1.0f / 30000.0f);
            mock_advance_cycle(16000U);
            app_foc_isr_step();
        }
        app_foc_run_once();
        app_inertia_identify_run_once();
    }

    app_inertia_identify_snapshot_t snap;
    app_inertia_identify_get_snapshot(&snap);
    CHECK(!snap.busy);
    CHECK(snap.state == APP_INERTIA_IDENTIFY_DONE);
    CHECK(snap.result_valid);
    CHECK(snap.point_count == APP_INERTIA_IDENTIFY_MAX_POINTS);
    CHECK(snap.run_count == APP_INERTIA_IDENTIFY_MAX_RUNS);
    CHECK(snap.valid_runs >= 6U);
    CHECK(snap.valid_forward_runs >= 3U);
    CHECK(snap.valid_reverse_runs >= 3U);
    CHECK(snap.median_j_total_kgm2 > 0.09f && snap.median_j_total_kgm2 < 0.14f);
    CHECK(snap.mad_j_total_kgm2 < 0.01f);
    CHECK(snap.result.j_total_kgm2 > 0.09f && snap.result.j_total_kgm2 < 0.14f);
    CHECK(snap.result.j_load_kgm2 > 0.03f && snap.result.j_load_kgm2 < 0.08f);
    app_foc_disable();
    s_plant_speed = 0.0f;
    g_mock_rotor_rad = 0.0f;
}
