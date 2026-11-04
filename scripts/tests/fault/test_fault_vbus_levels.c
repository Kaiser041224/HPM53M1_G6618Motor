/**
 * @file    test_fault_vbus_levels.c
 * @brief   L3 母线两级判据 + STOP/TRIP 分派 主机自测
 * @author  Kaiser
 *
 * 覆盖目标：
 *   ① 一级 OV（vbus_ov_v）连续 N 拍 → 动作 STOP（**不是** TRIP：卸转矩本身即降压）；
 *   ② 二级 OV（vbus_ov_trip_v）单拍越限 → 动作 TRIP（无去抖，硬件风险优先）；
 *   ③ UV → STOP；
 *   ④ 台架全 WARN（software.fault.actions_enable=0 的默认生效表）时二级亦不生效：
 *      41V 仍只按**一级去抖**计数告警，动作保持 WARN（行为与 v1 一致）；
 *   ⑤ STOP 在"无法软停机"状态（OFF-但桥使能 / CALIB / vtest）下由调用方降级 TRIP：
 *      以 app_fault_note_stop_rejected() 表达，动作升级为 TRIP。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_fault.h"
#include "test_util.h"

extern void mock_deps_reset(void);
extern void mock_params_defaults(void);
extern void mock_set_raw(adc_channel_t ch, uint16_t raw);
extern void mock_set_vbus_fast(float v);

static void setup(void) {
    mock_params_defaults();
    mock_deps_reset();
    mock_set_raw(ADC_CH_I_U, 32768U);
    mock_set_raw(ADC_CH_I_V, 32768U);
    mock_set_raw(ADC_CH_I_W, 32768U);
    app_fault_init(NULL);
}

/** @brief 推进 n 拍（跳过上电静默期后由调用方自行控制） */
static void tick_n(int n) {
    for (int i = 0; i < n; i++) {
        app_fault_tick();
    }
}

void test_fault_vbus_ov_two_levels(void) {
    setup();
    app_fault_set_vbus_levels(36.0f, 40.0f, 9.0f, 3U, 3U);
    app_fault_set_policy(APP_FAULT_VBUS_OV, APP_FLT_ACT_STOP);
    tick_n(60); /* 越过上电静默期 */

    /* ① 一级：连续 3 拍 36V 以上 → STOP（非 TRIP） */
    mock_set_vbus_fast(37.0f);
    tick_n(3);
    CHECK((app_fault_get_latched() & APP_FAULT_VBUS_OV) != 0U);
    CHECK(app_fault_get_action() == APP_FLT_ACT_STOP);
    CHECK(app_fault_get_state() != APP_FAULT_STATE_FAULT); /* STOP 不进 FAULT */

    /* 恢复 */
    mock_set_vbus_fast(24.0f);
    tick_n(5);
    CHECK(app_fault_clear() == 0);

    /* ② 二级：单拍 41V → TRIP（无去抖） */
    mock_set_vbus_fast(41.0f);
    tick_n(1);
    CHECK(app_fault_get_action() == APP_FLT_ACT_TRIP);

    mock_set_vbus_fast(24.0f);
    tick_n(5);
    CHECK(app_fault_clear() == 0);

    /* ③ UV：连续 3 拍 8V → STOP */
    app_fault_set_policy(APP_FAULT_VBUS_UV, APP_FLT_ACT_STOP);
    mock_set_vbus_fast(8.0f);
    tick_n(3);
    CHECK((app_fault_get_latched() & APP_FAULT_VBUS_UV) != 0U);
    CHECK(app_fault_get_action() == APP_FLT_ACT_STOP);
}

void test_fault_vbus_levels_bench_defaults(void) {
    setup();
    app_fault_set_vbus_levels(36.0f, 40.0f, 9.0f, 3U, 3U);
    tick_n(60);

    /* ④ 生效表全 WARN（台架保险）→ 41V 也只按一级去抖告警，动作保持 WARN */
    mock_set_vbus_fast(41.0f);
    tick_n(3);
    CHECK((app_fault_get_codes() & APP_FAULT_VBUS_OV) != 0U);
    CHECK(app_fault_get_action() == APP_FLT_ACT_WARN);
    CHECK(app_fault_get_state() != APP_FAULT_STATE_FAULT);
}

void test_fault_stop_rejected_escalates(void) {
    setup();
    app_fault_set_vbus_levels(36.0f, 40.0f, 9.0f, 3U, 3U);
    app_fault_set_policy(APP_FAULT_VBUS_OV, APP_FLT_ACT_STOP);
    tick_n(60);

    mock_set_vbus_fast(37.0f);
    tick_n(3);
    CHECK(app_fault_get_action() == APP_FLT_ACT_STOP);

    /* ⑤ 调用方报告"无法软停机" → 升级 TRIP */
    app_fault_note_stop_rejected();
    CHECK(app_fault_get_action() == APP_FLT_ACT_TRIP);
}
