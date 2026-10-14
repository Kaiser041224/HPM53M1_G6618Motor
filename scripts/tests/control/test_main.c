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
void test_foc_request(void);
void test_motion_timer(void);
void test_motion_math(void);
void test_motion(void);
void test_motion_sample_time(void);
void test_motion_restore_validation(void);
void test_motion_wrap_direction(void);

int main(void) {
    printf("Control host tests\n");
    test_foc_current_integration();
    test_foc_calib_port();
    test_id_rl();
    test_current_capture();
    test_current_identify();
    test_foc_stop();
    test_voltage_limit();
    test_foc_request();
    test_motion_timer();
    test_motion_math();
    test_motion();
    test_motion_sample_time();
    test_motion_restore_validation();
    test_motion_wrap_direction();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    if (g_checks == 0) {
        printf("ERROR: no checks executed (test wiring missing?)\n");
        return 1;
    }
    return (g_fails == 0) ? 0 : 1;
}
