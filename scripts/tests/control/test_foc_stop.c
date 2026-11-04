/** @file test_foc_stop.c
 * @brief 正常异步关闭契约测试
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_foc.h"
#include "app_foc_current.h"
#include "test_util.h"
extern void mock_params_reset(void);
void test_foc_stop(void) {
    mock_params_reset();
    app_foc_init();
    CHECK(app_foc_enable() == 0);
    CHECK(app_foc_set_iq_ref(4) == 0);
    app_foc_run_once();
    CHECK(app_foc_request_stop() == 0);
    CHECK(app_foc_stop_pending());
    CHECK(app_foc_current_vtest_start(0.5f,0,1)!=0);
    CHECK(app_foc_set_iq_ref(5) != 0);
    CHECK(app_foc_calib_begin() != 0);
    for (unsigned i = 0; i < 50; ++i)
        app_foc_run_once();
    CHECK(app_foc_get_state() != APP_FOC_STATE_OFF);
    CHECK(app_foc_get_iq_ref() > 0 && app_foc_get_iq_ref() < 4);
    for (unsigned i = 0; i < 100; ++i)
        app_foc_run_once();
    CHECK(app_foc_get_state() == APP_FOC_STATE_OFF);
    CHECK(!app_foc_stop_pending());
    CHECK(app_foc_enable() == 0);
    CHECK(app_foc_request_stop() == 0);
    app_foc_disable();
    CHECK(!app_foc_stop_pending());
}
