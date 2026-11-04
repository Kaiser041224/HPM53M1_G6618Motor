/** @file test_main.c
 * @brief DM CAN 协议编解码/策略主机自测入口
 * @author Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "test_util.h"

int g_checks = 0;
int g_fails = 0;

void test_can_dm_id(void);
void test_can_dm_mit(void);
void test_can_dm_control_float(void);
void test_can_dm_control_nan(void);
void test_can_dm_status_frames(void);
void test_can_dm_feedback(void);
void test_can_dm_torque_scale_split(void);
void test_can_dm_err_map(void);
void test_can_dm_scaling(void);
void test_can_dm_policy(void);

int main(void) {
    test_can_dm_id();
    test_can_dm_mit();
    test_can_dm_control_float();
    test_can_dm_control_nan();
    test_can_dm_status_frames();
    test_can_dm_feedback();
    test_can_dm_torque_scale_split();
    test_can_dm_err_map();
    test_can_dm_scaling();
    test_can_dm_policy();

    printf("DM CAN host tests\n%d checks, %d failures\n", g_checks, g_fails);
    return (g_fails == 0) ? 0 : 1;
}
