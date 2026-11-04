/**
 * @file    test_current_limit.c
 * @brief   FOC 内部电流 boost 管理器（连续 24.3A / 瞬态 30A）契约测试
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_foc_current.h"
#include "app_software_params.h"
#include "test_util.h"

extern void mock_params_reset(void);

/** @brief 连续限流 + I²t 型 boost 预算 + 回充滞环 */
void test_current_limit(void) {
    mock_params_reset();
    app_software_params_t* sw = app_software_params_mutable();

    sw->control.limits.i_q_max_a = 30.0f;
    sw->i_q_continuous_a = 24.3f;
    sw->i_q_boost_time_s = 2.0f;
    sw->i_q_boost_recover_time_s = 10.0f;
    sw->i_q_boost_recover_ratio = 0.9f;

    app_foc_current_init();
    CHECK_NEAR(app_foc_current_get_i_limit(), 30.0f, 1e-5f);

    /* 30A 满 boost 可维持 2s */
    app_foc_current_limit_update(30.0f, 1.0f);
    CHECK_NEAR(app_foc_current_get_i_limit(), 30.0f, 1e-5f);
    app_foc_current_limit_update(30.0f, 1.01f);
    CHECK_NEAR(app_foc_current_get_i_limit(), 24.3f, 1e-5f);

    /* 处于连续限流上沿时不回充，避免 30A/24.3A 反复脉冲 */
    app_foc_current_limit_update(24.3f, 5.0f);
    CHECK_NEAR(app_foc_current_get_i_limit(), 24.3f, 1e-5f);

    /* 低于 0.9×连续值才回充；回充完成后 boost 再次可用 */
    app_foc_current_limit_update(20.0f, 5.0f);
    CHECK_NEAR(app_foc_current_get_i_limit(), 30.0f, 1e-5f);
    app_foc_current_limit_update(20.0f, 5.0f);
    CHECK_NEAR(app_foc_current_get_i_limit(), 30.0f, 1e-5f);

    /* 再次耗尽 */
    app_foc_current_limit_update(30.0f, 2.0f);
    CHECK_NEAR(app_foc_current_get_i_limit(), 24.3f, 1e-5f);

    /* boost_time=0：直接使用连续限幅 */
    sw->i_q_boost_time_s = 0.0f;
    app_foc_current_init();
    CHECK_NEAR(app_foc_current_get_i_limit(), 24.3f, 1e-5f);
}
