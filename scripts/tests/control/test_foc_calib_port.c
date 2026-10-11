/**
 * @file    test_foc_calib_port.c
 * @brief   app_foc 校准端口（真实实现）主机自测
 * @author  Kaiser
 *
 * 覆盖（断言以 ISR 实际计算的 θe 为准，而非仅看镜像变量）：
 *   1) 状态门控：OFF 拒绝 begin；非 CALIB 拒绝 set_*；vtest 进行中拒绝 begin
 *   2) 试用零点只影响 CALIB 期间的角度链，运行零点镜像不变；end 后 ISR 角度回到运行零点
 *   3) apply 在 CALIB 中拒绝；READY 中生效（ISR 角度随之改变）
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

#include "app_foc.h"
#include "app_foc_current.h"
#include "foc_math.h"

extern void mock_params_reset(void);
extern float g_mock_rotor_rad;

/** 跑一拍 ISR 并返回快照中的 θe */
static float isr_theta_e(void) {
    app_foc_current_snapshot_t snap;

    app_foc_isr_step();
    app_foc_get_snapshot(&snap);
    return snap.theta_e_rad;
}

void test_foc_calib_port(void) {
    float offset;
    float dir;
    const float theta_m = 0.1f; /* p=10 → p·θm = 1.0 rad */

    mock_params_reset();
    g_mock_rotor_rad = theta_m;
    app_foc_init();

    /* 1) OFF：begin/set_* 拒绝 */
    CHECK(app_foc_calib_begin() != 0);
    CHECK(app_foc_calib_set_excitation(0.0f, 1.0f, 0.0f) != 0);
    CHECK(app_foc_calib_use_trial_offset(1.0f, 1.0f) != 0);
    CHECK(app_foc_calib_set_current(1.0f, 0.0f) != 0);

    CHECK(app_foc_enable() == 0);
    CHECK(app_foc_get_state() == APP_FOC_STATE_READY);
    app_foc_get_encoder_offset(&offset, &dir);
    CHECK_NEAR(offset, 0.0f, 1e-6f);
    CHECK_NEAR(dir, 1.0f, 1e-6f);

    /* 3a) READY 中 apply：镜像与 ISR 角度同时生效 */
    CHECK(app_foc_apply_encoder_offset(0.5f, -1.0f) == 0);
    CHECK(app_foc_apply_encoder_offset(0.5f, 0.5f) != 0); /* 非 ±1 拒绝 */
    app_foc_get_encoder_offset(&offset, &dir);
    CHECK_NEAR(offset, 0.5f, 1e-6f);
    CHECK_NEAR(dir, -1.0f, 1e-6f);
    CHECK_NEAR(isr_theta_e(), foc_wrap_2pi(-1.0f - 0.5f), 1e-4f);

    /* 2) begin → 强制角 */
    CHECK(app_foc_calib_begin() == 0);
    CHECK(app_foc_get_state() == APP_FOC_STATE_CALIB);
    CHECK(app_foc_calib_begin() != 0); /* 重入拒绝 */
    CHECK(app_foc_calib_set_excitation(1.25f, 2.0f, 0.0f) == 0);
    CHECK_NEAR(isr_theta_e(), 1.25f, 1e-4f);

    /* 3b) CALIB 中 apply 拒绝（试用值不会被固化为运行值） */
    CHECK(app_foc_apply_encoder_offset(3.0f, 1.0f) != 0);

    /* 2) 试用零点：ISR 改用编码器角 + 试用零点；运行镜像不变 */
    CHECK(app_foc_calib_use_trial_offset(2.0f, 1.0f) == 0);
    CHECK(app_foc_calib_set_current(2.0f, 0.0f) == 0);
    CHECK_NEAR(isr_theta_e(), foc_wrap_2pi(1.0f - 2.0f), 1e-4f);
    app_foc_get_encoder_offset(&offset, &dir);
    CHECK_NEAR(offset, 0.5f, 1e-6f);
    CHECK_NEAR(dir, -1.0f, 1e-6f);

    /* 2) end：READY，ISR 角度回到运行零点（试用值作废） */
    app_foc_calib_end();
    CHECK(app_foc_get_state() == APP_FOC_STATE_READY);
    CHECK_NEAR(isr_theta_e(), foc_wrap_2pi(-1.0f - 0.5f), 1e-4f);
    CHECK(app_foc_calib_set_current(1.0f, 0.0f) != 0); /* 非 CALIB 拒绝 */
    app_foc_calib_end();                               /* 非 CALIB：无操作 */
    CHECK(app_foc_get_state() == APP_FOC_STATE_READY);

    /* 1) vtest 进行中：begin 拒绝 */
    CHECK(app_foc_current_vtest_start(0.5f, 0.0f, 1.0f) == 0);
    CHECK(app_foc_calib_begin() != 0);
    app_foc_current_vtest_stop();
    CHECK(app_foc_calib_begin() == 0);

    /* CALIB 中被关闭：OFF；重新使能后零点由电机参数重建（apply 未写参数 → 0） */
    app_foc_disable();
    CHECK(app_foc_get_state() == APP_FOC_STATE_OFF);
    CHECK(app_foc_enable() == 0);
    app_foc_get_encoder_offset(&offset, &dir);
    CHECK_NEAR(offset, 0.0f, 1e-6f);
    CHECK_NEAR(dir, 1.0f, 1e-6f);
    app_foc_disable();
}
