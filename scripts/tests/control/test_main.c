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
void test_motion_speed_anti_windup(void);
void test_motion(void);
void test_motion_sample_time(void);
void test_motion_restore_validation(void);
void test_motion_wrap_direction(void);
void test_motion_hold(void);
void test_motion_anti_windup(void);
void test_motion_cmd_status(void);
void test_motion_friction_feedforward(void);
void test_motion_params(void);
void test_motion_vernier_fallback(void);
void test_friction_math(void);
void test_friction_identify(void);
void test_friction_identify_failure(void);
void test_friction_identify_tolerance(void);
void test_friction_identify_diagnostics(void);

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
    test_motion_speed_anti_windup();
    test_motion();
    test_motion_sample_time();
    test_motion_restore_validation();
    test_motion_wrap_direction();
    test_motion_hold();
    test_motion_anti_windup();
    test_motion_cmd_status();
    test_motion_friction_feedforward();
    test_motion_params();
    test_motion_vernier_fallback();
    test_friction_math();
    test_friction_identify();
    test_friction_identify_failure();
    test_friction_identify_tolerance();
    test_friction_identify_diagnostics();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    if (g_checks == 0) {
        printf("ERROR: no checks executed (test wiring missing?)\n");
        return 1;
    }
    return (g_fails == 0) ? 0 : 1;
}
