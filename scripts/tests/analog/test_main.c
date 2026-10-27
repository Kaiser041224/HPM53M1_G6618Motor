/**
 * @file    test_main.c
 * @brief   模拟量层主机自测入口
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

int g_checks = 0;
int g_fails = 0;

void test_analog_vbus_fast_scale(void);
void test_analog_vbus_fast_filter_rate(void);
void test_analog_vbus_fast_isolates_slow(void);
void test_analog_vbus_fast_unavailable(void);

int main(void) {
    test_analog_vbus_fast_scale();
    test_analog_vbus_fast_filter_rate();
    test_analog_vbus_fast_isolates_slow();
    test_analog_vbus_fast_unavailable();

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return (g_fails == 0) ? 0 : 1;
}
