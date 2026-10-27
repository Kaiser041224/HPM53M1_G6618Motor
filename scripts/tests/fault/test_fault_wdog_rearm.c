/**
 * @file    test_fault_wdog_rearm.c
 * @brief   L1 WDOG 自动重臂 + 次数上限 主机自测
 * @author  Kaiser
 *
 * 覆盖目标（消除 v1 spec §10 的"L1 WDOG 为一次性"缺口）：
 *   ① 命中后经 `rearm_ms` 且三相 raw 已回到窗内 → 自动重臂一次（无需 fault clear）；
 *   ② raw 未回窗 → 无论等多久都不重臂（避免持续过流下反复重臂）；
 *   ③ 单次上电重臂次数达上限 → 不再自动重臂（升 TRIP 锁存，仅 fault clear 复位）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_fault.h"
#include "test_util.h"

extern void mock_deps_reset(void);
extern void mock_params_defaults(void);
extern void mock_set_raw(adc_channel_t ch, uint16_t raw);
extern int g_mock_wdog_reenable_calls;

#define RAW_IN_WINDOW (32768U)
#define RAW_OVER_HI   (60000U)

/** @brief 初始化并让三相 raw 处于窗内 */
static void setup(void) {
    mock_params_defaults();
    mock_deps_reset();
    mock_set_raw(ADC_CH_I_U, RAW_IN_WINDOW);
    mock_set_raw(ADC_CH_I_V, RAW_IN_WINDOW);
    mock_set_raw(ADC_CH_I_W, RAW_IN_WINDOW);
    app_fault_init(NULL);
    /* 越过上电静默期（50 拍）：静默期内 WDOG 被刻意忽略（上电瞬态假触发防护） */
    for (int i = 0; i < 60; i++) {
        app_fault_tick();
    }
}

/** @brief 推进 n 次重臂服务（模拟 1kHz 任务调用） */
static void service_n(int n) {
    for (int i = 0; i < n; i++) {
        app_fault_wdog_service();
    }
}

void test_fault_wdog_rearm_after_transient(void) {
    setup();
    app_fault_set_wdog_rearm(2U, 5U); /* 2ms 后可重臂，上限 5 次 */

    /* 单次瞬态：U 相越窗命中 */
    app_fault_on_wdog(ADC_CH_I_U, RAW_OVER_HI, NULL);
    CHECK((app_fault_get_latched() & APP_FAULT_OC_FAST_U) != 0U);

    /* 重臂延时未到 → 不重臂 */
    service_n(2);
    CHECK(g_mock_wdog_reenable_calls == 0);

    /* 延时到且 raw 已回窗 → 自动重臂一次 */
    service_n(1);
    CHECK(g_mock_wdog_reenable_calls == 1);
    CHECK(app_fault_get_wdog_rearm_count(0U) == 1U);

    /* 已重臂：后续不再重复重臂 */
    service_n(10);
    CHECK(g_mock_wdog_reenable_calls == 1);
}

void test_fault_wdog_no_rearm_while_out_of_window(void) {
    setup();
    app_fault_set_wdog_rearm(2U, 5U);

    app_fault_on_wdog(ADC_CH_I_W, RAW_OVER_HI, NULL);
    /* 电流持续超窗（真实过流）→ 无论等多久都不重臂 */
    mock_set_raw(ADC_CH_I_W, RAW_OVER_HI);
    service_n(50);
    CHECK(g_mock_wdog_reenable_calls == 0);
    CHECK(app_fault_get_wdog_rearm_count(2U) == 0U);

    /* 电流回窗后才允许重臂 */
    mock_set_raw(ADC_CH_I_W, RAW_IN_WINDOW);
    service_n(2);
    CHECK(g_mock_wdog_reenable_calls == 1);
}

void test_fault_wdog_rearm_limit_escalates_trip(void) {
    setup();
    app_fault_set_wdog_rearm(1U, 2U); /* 上限 2 次 */

    /* 第 1 次瞬态 → 重臂 */
    app_fault_on_wdog(ADC_CH_I_U, RAW_OVER_HI, NULL);
    service_n(2);
    CHECK(g_mock_wdog_reenable_calls == 1);

    /* 第 2 次瞬态 → 重臂 */
    app_fault_on_wdog(ADC_CH_I_U, RAW_OVER_HI, NULL);
    service_n(2);
    CHECK(g_mock_wdog_reenable_calls == 2);

    /* 第 3 次瞬态 → 达上限：不再自动重臂（锁存，仅 fault clear 复位） */
    app_fault_on_wdog(ADC_CH_I_U, RAW_OVER_HI, NULL);
    service_n(20);
    CHECK(g_mock_wdog_reenable_calls == 2);
    CHECK(app_fault_get_wdog_rearm_count(0U) == 2U);
}

/**
 * @brief 上电静默期内 WDOG 必须被忽略
 *
 * 实测依据（2026-10-27 台架）：启动瞬态下 PMT 结果整帧可能为 0/全 1（快照 vbus=0、
 * 三相电流=0），硬件 WDOG 会假触发并锁存三路 OC_FAST → actions 打开时每次开机
 * 直接进 FAULT、拒绝 foc on。
 */
void test_fault_wdog_ignored_during_settle(void) {
    mock_params_defaults();
    mock_deps_reset();
    mock_set_raw(ADC_CH_I_U, RAW_IN_WINDOW);
    app_fault_init(NULL);

    /* 静默期内（未推进 settle_ticks）注入 WDOG 命中 → 不锁存、不记录 */
    app_fault_on_wdog(ADC_CH_I_U, RAW_OVER_HI, NULL);
    CHECK((app_fault_get_latched() & APP_FAULT_OC_FAST_U) == 0U);
    CHECK(app_fault_get_first() == 0U);

    /* 越过静默期后 → 正常记录 */
    for (int i = 0; i < 60; i++) {
        app_fault_tick();
    }
    app_fault_on_wdog(ADC_CH_I_V, RAW_OVER_HI, NULL);
    CHECK((app_fault_get_latched() & APP_FAULT_OC_FAST_V) != 0U);
}
