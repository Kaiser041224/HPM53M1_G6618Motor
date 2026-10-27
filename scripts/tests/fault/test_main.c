/**
 * @file    test_main.c
 * @brief   保护链（app_fault）主机自测入口
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

int g_checks = 0;
int g_fails = 0;

void test_fault_wdog_rearm_after_transient(void);
void test_fault_wdog_ignored_during_settle(void);
void test_fault_wdog_no_rearm_while_out_of_window(void);
void test_fault_wdog_rearm_limit_escalates_trip(void);
void test_fault_policy_defaults(void);
void test_fault_vbus_ov_two_levels(void);
void test_fault_vbus_levels_bench_defaults(void);
void test_fault_stop_rejected_escalates(void);
void test_fault_policy_action_levels(void);
void test_fault_policy_runtime_override(void);
void test_fault_derate_releases_with_condition(void);
void test_fault_actions_enable_switch(void);
void test_fault_inject_sets_first(void);
void test_fault_inject_stop_requests_stop(void);

int main(void) {
    test_fault_wdog_rearm_after_transient();
    test_fault_wdog_ignored_during_settle();
    test_fault_wdog_no_rearm_while_out_of_window();
    test_fault_wdog_rearm_limit_escalates_trip();
    test_fault_policy_defaults();
    test_fault_vbus_ov_two_levels();
    test_fault_vbus_levels_bench_defaults();
    test_fault_stop_rejected_escalates();
    test_fault_policy_action_levels();
    test_fault_policy_runtime_override();
    test_fault_derate_releases_with_condition();
    test_fault_actions_enable_switch();
    test_fault_inject_sets_first();
    test_fault_inject_stop_requests_stop();

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return (g_fails == 0) ? 0 : 1;
}
