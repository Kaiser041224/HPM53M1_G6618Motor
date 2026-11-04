/**
 * @file    test_foc_fault_contract.c
 * @brief   FOC 与动作层的接口契约（使能拒绝 / 快跳登记）主机自测
 * @author  Kaiser
 *
 * 覆盖目标：
 *   ① `app_foc_enable()` 仅在"锁存带动作码（STOP/TRIP）"时拒绝；纯 WARN/DERATE 不阻塞
 *      （消除"nuisance ENC_READ 挡住使能"的问题）；
 *   ② `app_foc_request_fault_stop()`：OFF / CALIB / vtest 下返回 -1（调用方降级 TRIP），
 *      READY 下返回 0 并复用既有软停机状态机；
 *   ③ (未在本套件覆盖) 电流环快跳登记 `APP_FAULT_OC_TRIP` —— 需在台架用
 *      `param set software.control.limits.i_trip_a <A>` 触发后看 `fault show` 出现
 *      code=0x400；主机侧无法构造 is_tripped 条件。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

#include "app_fault.h"
#include "app_foc.h"

extern void mock_params_reset(void);
extern uint32_t g_mock_fault_stop_request;
extern uint32_t g_mock_fault_stop_rejected_calls;
extern float g_mock_fault_limit_scale;
extern uint32_t g_mock_fault_tick_calls;
extern int g_mock_inverter_enabled;
extern uint32_t g_mock_fault_blocking;
extern uint32_t g_mock_fault_raise_calls;

void test_foc_enable_blocked_only_by_action_codes(void) {
    mock_params_reset();
    app_foc_init();

    /* 无带动作码 → 使能成功 */
    g_mock_fault_blocking = 0U;
    CHECK(app_foc_enable() == 0);
    app_foc_disable();

    /* 锁存带动作码 → 拒绝 */
    g_mock_fault_blocking = APP_FAULT_VBUS_OV;
    CHECK(app_foc_enable() == -2);
    g_mock_fault_blocking = 0U;
    CHECK(app_foc_enable() == 0);
    app_foc_disable();
}

void test_foc_fault_stop_request_paths(void) {
    mock_params_reset();
    app_foc_init();
    app_foc_disable();

    /* OFF 且桥已断（正常空闲）：无需停机，返回 0（**不算失败**，不触发升级） */
    CHECK(app_foc_request_fault_stop(APP_FAULT_VBUS_OV) == 0);

    /* READY：可发起受控停机 */
    CHECK(app_foc_enable() == 0);
    CHECK(app_foc_request_fault_stop(APP_FAULT_VBUS_OV) == 0);
    CHECK(app_foc_stop_pending());
    app_foc_disable();
    CHECK(!app_foc_stop_pending());
    CHECK(app_foc_request_fault_stop(APP_FAULT_VBUS_OV) == 0);
}

void test_foc_stop_request_dispatch(void) {
    mock_params_reset();
    app_foc_init();
    app_foc_disable();

    /* OFF 且桥已断（正常空闲）：无需停机，**不算失败**（不升级 TRIP） */
    g_mock_inverter_enabled = 0;
    g_mock_fault_stop_request = APP_FAULT_VBUS_OV;
    g_mock_fault_stop_rejected_calls = 0U;
    app_foc_run_once();
    CHECK(g_mock_fault_stop_rejected_calls == 0U);
    CHECK(g_mock_fault_stop_request == 0U); /* 边沿：已被取走 */

    /* OFF **但桥仍带电**（debug inverter 50% 输出）= 真危险态 → 报告"不可用"升级 TRIP */
    g_mock_inverter_enabled = 1;
    g_mock_fault_stop_request = APP_FAULT_VBUS_OV;
    g_mock_fault_stop_rejected_calls = 0U;
    app_foc_run_once();
    CHECK(g_mock_fault_stop_rejected_calls == 1U);
    CHECK(g_mock_fault_stop_request == 0U); /* 边沿：已被取走 */
    g_mock_inverter_enabled = 0;
}

void test_foc_current_limit_scales_with_derate(void) {
    float base;

    mock_params_reset();
    app_foc_current_init();
    g_mock_fault_limit_scale = 1.0f;
    base = app_foc_current_get_i_limit();
    CHECK(base > 0.0f);

    /* DERATE 生效 → iq 上限按倍率下降（FOC 与 motion 共用本函数，自动传播） */
    g_mock_fault_limit_scale = 0.5f;
    CHECK_NEAR(app_foc_current_get_i_limit(), base * 0.5f, 1e-4f);

    g_mock_fault_limit_scale = 1.0f;
    CHECK_NEAR(app_foc_current_get_i_limit(), base, 1e-4f);
}

void test_foc_wait_off_pumps_protection(void) {
    mock_params_reset();
    app_foc_init();
    app_foc_enable();
    CHECK(app_foc_request_stop() == 0);

    /* wait_off 自旋期间必须泵保护（否则减速/软关断窗口内慢保护停摆，而该窗口最易反灌） */
    g_mock_fault_tick_calls = 0U;
    (void)app_foc_wait_off(500U);
    CHECK(g_mock_fault_tick_calls > 0U);
}
