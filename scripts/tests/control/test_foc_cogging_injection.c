/**
 * @file    test_foc_cogging_injection.c
 * @brief   FOC 电流环 q 轴齿槽前馈注入与 current_limited 基准测试
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_cogging.h"
#include "app_foc.h"
#include "app_motor_params.h"
#include "test_util.h"

extern void mock_params_reset(void);
extern float g_mock_rotor_rad;

static void cogging_test_tables(app_cogging_tables_t* tables) {
    *tables = (app_cogging_tables_t){0};
    for (uint16_t i = 0U; i < ALGO_COGGING_ROTOR_POINTS; i++) {
        tables->rotor[i] = 100; /* 100 × 0.0005 / 0.1 = 0.5A */
    }
    tables->rotor_valid = true;
    tables->rotor_stats.valid = true;
    tables->rotor_stats.valid_bins = ALGO_COGGING_ROTOR_POINTS;
}

static void submit_iq(float iq, uint32_t sequence) {
    app_foc_current_request_t request = {
        .id_ref_a = 0.0f,
        .iq_ref_a = iq,
        .sequence = sequence,
        .epoch = app_foc_current_epoch(),
        .valid = true,
    };
    CHECK(app_foc_submit_current(&request) == 0);
}

void test_foc_cogging_injection(void) {
    app_cogging_tables_t tables;

    mock_params_reset();
    app_motor_params_mutable()->kt_nm_per_a = 0.1f;
    app_motor_params_mutable()->reducer_ratio = 50.0f;
    app_foc_init();
    app_cogging_init();
    app_cogging_set_output_angle(0.0f, false);
    cogging_test_tables(&tables);
    CHECK(app_cogging_set_tables(&tables) == 0);
    CHECK(app_cogging_set_master(true) == 0);
    CHECK(app_cogging_set_rotor_enable(true) == 0);
    app_cogging_set_motion_active(true);

    CHECK(app_foc_enable() == 0);
    CHECK(app_foc_set_current_source(APP_FOC_CURRENT_SOURCE_MOTION) == 0);
    g_mock_rotor_rad = 0.0f;
    submit_iq(2.0f, 1U);
    app_foc_isr_step();
    app_foc_execution_t feedback;
    app_foc_get_execution(&feedback);
    CHECK(feedback.valid);
    CHECK_NEAR(feedback.requested_iq_a, 2.5f, 1e-5f);
    CHECK_NEAR(feedback.accepted_iq_a, 2.5f, 1e-5f);
    CHECK(!feedback.current_limited);
    CHECK_NEAR(g_foc_current_snapshot.i_q_cogging_a, 0.5f, 1e-5f);

    /* 关表后同一物理请求回到 2A。 */
    CHECK(app_cogging_set_master(false) == 0);
    submit_iq(2.0f, 2U);
    app_foc_isr_step();
    app_foc_get_execution(&feedback);
    CHECK_NEAR(feedback.requested_iq_a, 2.0f, 1e-5f);
    CHECK_NEAR(feedback.accepted_iq_a, 2.0f, 1e-5f);
    CHECK_NEAR(g_foc_current_snapshot.i_q_cogging_a, 0.0f, 1e-6f);
    app_foc_disable();
}
