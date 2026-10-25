/**
 * @file    test_foc_model_apply.c
 * @brief   FOC 电流环模型参数重载契约测试（R·I 前馈跟随 param 修改）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_foc.h"
#include "app_foc_current.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "test_util.h"

extern void mock_params_reset(void);
extern void mock_set_current_gains(float kp, float ki);
extern float g_mock_duty[3];

/** @brief d 轴 1A 反馈：Clarke/Park 在 θe=0 下得到 id=1、iq=0 */
static int run_id_one_amp(float* v_d_out) {
    bool saturated = false;
    int rc = app_foc_current_run_fresh(
        0.0f, 0.0f, 1.0f, 0.0f, 1.0f, -0.5f, -0.5f, 24.0f, g_mock_duty, &saturated);
    app_foc_current_snapshot_t snapshot;
    app_foc_current_get_snapshot(&snapshot);
    if (v_d_out != NULL) {
        *v_d_out = snapshot.v_d_v;
    }
    return rc;
}

void test_foc_model_apply(void) {
    float v_d = 0.0f;

    mock_params_reset();
    mock_set_current_gains(1.0f, 0.0f);
    app_software_params_mutable()->current_ff.r_en = 1U;
    app_motor_params_mutable()->rs_ohm = 0.2f;

    app_foc_current_init();
    CHECK(app_foc_current_is_ready());
    app_foc_current_reset();
    CHECK(run_id_one_amp(&v_d) == 0);
    CHECK_NEAR(v_d, 0.2f, 1e-4f);

    /* 直接改参数后必须显式重载模型，否则 R·I 前馈仍使用旧电阻。 */
    app_motor_params_mutable()->rs_ohm = 0.5f;
    CHECK(app_foc_current_reload_model() == 0);
    app_foc_current_reset();
    CHECK(run_id_one_amp(&v_d) == 0);
    CHECK_NEAR(v_d, 0.5f, 1e-4f);

    /* 非法模型值不得破坏已加载模型。 */
    app_motor_params_mutable()->rs_ohm = NAN;
    CHECK(app_foc_current_reload_model() != 0);
    app_foc_current_reset();
    CHECK(run_id_one_amp(&v_d) == 0);
    CHECK_NEAR(v_d, 0.5f, 1e-4f);
}

/** @brief foc on 端到端：disable 后改参数，下一次 enable 必须重载 R/L/λ 模型 */
void test_foc_model_apply_enable(void) {
    float v_d = 0.0f;

    mock_params_reset();
    mock_set_current_gains(1.0f, 0.0f);
    app_software_params_mutable()->current_ff.r_en = 1U;
    app_motor_params_mutable()->rs_ohm = 0.2f;
    app_motor_params_mutable()->kt_nm_per_a = 0.117f;
    app_foc_init();
    CHECK(app_foc_enable() == 0);
    app_foc_disable();

    app_motor_params_mutable()->rs_ohm = 0.5f;
    CHECK(app_foc_enable() == 0);
    app_foc_current_reset();
    CHECK(run_id_one_amp(&v_d) == 0);
    CHECK_NEAR(v_d, 0.5f, 1e-4f);
    app_foc_disable();
}
