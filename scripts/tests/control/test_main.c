/**
 * @file    test_main.c
 * @brief   控制层（app_foc_current）主机自测入口
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

int g_checks = 0;
int g_fails = 0;

void test_foc_current_integration(void);
void test_foc_calib_port(void);
void test_id_rl(void);
void test_current_capture(void);
void test_current_identify(void);
void test_foc_stop(void);
void test_voltage_limit(void);

int main(void) {
    printf("Control host tests\n");
    test_foc_current_integration();
    test_foc_calib_port();
    test_id_rl();
    test_current_capture();
    test_current_identify();
    test_foc_stop();
    test_voltage_limit();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    if (g_checks == 0) {
        printf("ERROR: no checks executed (test wiring missing?)\n");
        return 1;
    }
    return (g_fails == 0) ? 0 : 1;
}
