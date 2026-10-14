/** @file test_foc_request.c
 * @brief 统一电流请求权限与执行契约
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_foc.h"
#include "test_util.h"
extern void mock_params_reset(void);
void test_foc_request(void) {
    mock_params_reset();
    app_foc_init();
    CHECK(app_foc_enable() == 0);
    CHECK(app_foc_set_current_source(APP_FOC_CURRENT_SOURCE_MOTION) == 0);
    CHECK(app_foc_current_vtest_start(.5f, 0, 1) != 0);
    app_foc_current_vtest_stop(); /* 失败实现也不污染后续独立断言 */
    app_foc_current_request_t request = {
        .id_ref_a = 1,
        .iq_ref_a = 2,
        .sequence = 7,
        .epoch = app_foc_current_epoch(),
        .valid = true};
    CHECK(app_foc_submit_current(&request) == 0);
    CHECK(app_foc_current_vtest_start(.5f, 0, 1) != 0);
    CHECK(app_foc_set_id_ref(2) != 0);
    CHECK(app_foc_set_current_source(APP_FOC_CURRENT_SOURCE_DIRECT) != 0);
    CHECK(app_foc_set_iq_ref(9) != 0);
    CHECK(app_foc_set_angle_source(APP_FOC_ANGLE_FORCED, 0) != 0);
    app_foc_run_once();
    app_foc_isr_step();
    app_foc_execution_t feedback;
    app_foc_get_execution(&feedback);
    CHECK(feedback.request_sequence == 7);
    CHECK(feedback.valid);
    CHECK_NEAR(feedback.accepted_id_a, 1, 1e-6f);
    CHECK_NEAR(feedback.accepted_iq_a, 2, 1e-6f);
    CHECK(app_foc_get_state() == APP_FOC_STATE_RUN);
    request.id_ref_a = 100;
    request.iq_ref_a = 100;
    CHECK(app_foc_submit_current(&request) == 0);
    app_foc_isr_step();
    app_foc_get_execution(&feedback);
    CHECK(feedback.current_limited && feedback.limited_count > 0);
    volatile uint32_t estop = 1;
    app_foc_register_estop_request(&estop);
    app_foc_isr_step();
    app_foc_get_execution(&feedback);
    CHECK(!feedback.valid);
    app_foc_register_estop_request(NULL);
    app_foc_disable();
    CHECK(app_foc_enable() == 0);
    request.epoch = app_foc_current_epoch();
    request.id_ref_a = 1;
    request.iq_ref_a = 2;
    request.iq_ref_a = NAN;
    CHECK(app_foc_submit_current(&request) != 0);
    request.iq_ref_a = 2;
    CHECK(app_foc_calib_begin() == 0);
    CHECK(app_foc_submit_current(&request) != 0);
    app_foc_calib_end();
    CHECK(app_foc_submit_current(&request) != 0);
    request.epoch = app_foc_current_epoch();
    app_foc_isr_step();
    app_foc_get_execution(&feedback);
    CHECK_NEAR(feedback.requested_iq_a, 0, 1e-6f);
    request.epoch = app_foc_current_epoch();
    CHECK(app_foc_submit_current(&request) == 0);
    /* mock当前mcycle为0；通过实时时钟推进验证请求租约。 */
    extern void mock_advance_cycle(uint32_t cycles);
    mock_advance_cycle(480000U);
    app_foc_isr_step();
    app_foc_get_execution(&feedback);
    CHECK(feedback.request_expired);
    CHECK_NEAR(feedback.accepted_iq_a, 0, 1e-6f);
    /* mcycle回到提交时刻附近，不得使已过期请求复活。 */
    mock_advance_cycle(UINT32_MAX - 480000U + 1U);
    app_foc_isr_step();
    app_foc_get_execution(&feedback);
    CHECK(feedback.request_expired);
    CHECK_NEAR(feedback.accepted_iq_a, 0, 1e-6f);
    CHECK(app_foc_submit_current(&request) == 0);
    app_foc_isr_step();
    app_foc_get_execution(&feedback);
    CHECK(!feedback.request_expired);
    CHECK_NEAR(feedback.accepted_iq_a, 2, 1e-6f);
    CHECK(app_foc_request_stop() == 0);
    CHECK(app_foc_submit_current(&request) != 0);
    app_foc_disable();
    CHECK(app_foc_enable() == 0);
    CHECK(app_foc_submit_current(&request) != 0);
    app_foc_isr_step();
    app_foc_get_execution(&feedback);
    CHECK_NEAR(feedback.accepted_iq_a, 0, 1e-6f);
    app_foc_disable();
    CHECK(app_foc_set_current_source(APP_FOC_CURRENT_SOURCE_DIRECT) == 0);
    CHECK(app_foc_enable() == 0);
    CHECK(app_foc_set_angle_source(APP_FOC_ANGLE_FORCED, 0) == 0);
    CHECK(app_foc_set_current_source(APP_FOC_CURRENT_SOURCE_MOTION) != 0);
    app_foc_disable();
}
