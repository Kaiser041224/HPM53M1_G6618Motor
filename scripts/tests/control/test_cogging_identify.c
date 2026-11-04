/**
 * @file    test_cogging_identify.c
 * @brief   cal cogging 启动守卫测试
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_cogging_identify.h"
#include "app_foc.h"
#include "app_motion.h"
#include "app_motor_params.h"
#include "test_util.h"

extern void mock_params_reset(void);

void test_cogging_identify_start(void) {
    mock_params_reset();
    app_motor_params_mutable()->kt_nm_per_a = 0.117f;
    app_motor_params_mutable()->reducer_ratio = 50.0f;
    app_foc_init();
    app_motion_init();
    app_cogging_init();
    app_cogging_identify_init();

    /* FOC OFF 拒绝。 */
    CHECK(app_cogging_identify_start(0.0f, 0U) == -2);
    CHECK(app_cogging_identify_start(2.0f, 4U) == -2);

    /* FOC READY 但 motion 未标定，拒绝进入标定。 */
    CHECK(app_foc_enable() == 0);
    CHECK(app_cogging_identify_start(2.0f, 4U) == -3);
    app_cogging_identify_abort();
    app_foc_disable();
}
