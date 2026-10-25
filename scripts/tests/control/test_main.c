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

void test_current_limit(void);
void test_foc_current_integration(void);
void test_foc_model_apply(void);
void test_foc_model_apply_enable(void);
void test_foc_speed_observer(void);
void test_algo_speed_observer(void);
void test_foc_calib_port(void);
void test_id_rl(void);
void test_current_capture(void);
void test_current_identify(void);
void test_foc_stop(void);
void test_voltage_limit(void);
void test_foc_request(void);
void test_motion_timer(void);
void test_motion_timer_tick_bind(void);
void test_motion_math(void);
void test_algo_ramp_plan(void);
void test_algo_inertia(void);
void test_algo_cogging(void);
void test_app_cogging(void);
void test_app_cogging_defaults(void);
void test_cogging_identify_start(void);
void test_cogging_store(void);
void test_foc_cogging_injection(void);
void test_inertia_identify_start(void);
void test_inertia_identify_simulation(void);
void test_motion_pos_filter(void);
void test_motion_pos_filter_accel(void);
void test_motion_pos_filter_bw_clamp(void);
void test_motion_position_p(void);
void test_motion_position_circular_p(void);
void test_motion_pos_filter_circular(void);
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
void test_motion_output_direction(void);
void test_motion_speed_rotor_only(void);
void test_motion_position_command(void);
void test_motion_position_gains(void);
void test_motion_position_cascade(void);
void test_motion_position_circular(void);
void test_motion_position_ff(void);
void test_motion_position_trajectory(void);
void test_motion_position_replan_velocity(void);
void test_motion_inertia_feedforward(void);
void test_motion_speed_inertia_direct_no_ff(void);
void test_motion_inertia_feedforward_filter(void);
void test_motion_speed_limit_app(void);
void test_motion_torque_ramp_app(void);
void test_motion_ramp_and_limit(void);
void test_friction_math(void);
void test_friction_identify(void);
void test_friction_identify_failure(void);
void test_friction_identify_tolerance(void);
void test_friction_identify_diagnostics(void);

int main(void) {
    printf("Control host tests\n");
    test_current_limit();
    test_foc_current_integration();
    test_foc_model_apply();
    test_foc_model_apply_enable();
    test_foc_speed_observer();
    test_algo_speed_observer();
    test_foc_calib_port();
    test_id_rl();
    test_current_capture();
    test_current_identify();
    test_foc_stop();
    test_voltage_limit();
    test_foc_request();
    test_motion_timer();
    test_motion_timer_tick_bind();
    test_motion_math();
    test_algo_ramp_plan();
    test_algo_inertia();
    test_algo_cogging();
    test_app_cogging();
    test_app_cogging_defaults();
    test_cogging_identify_start();
    test_cogging_store();
    test_foc_cogging_injection();
    test_inertia_identify_start();
    test_inertia_identify_simulation();
    test_motion_pos_filter();
    test_motion_pos_filter_accel();
    test_motion_pos_filter_bw_clamp();
    test_motion_position_p();
    test_motion_position_circular_p();
    test_motion_pos_filter_circular();
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
    test_motion_output_direction();
    test_motion_speed_rotor_only();
    test_motion_position_command();
    test_motion_position_gains();
    test_motion_position_cascade();
    test_motion_position_circular();
    test_motion_position_ff();
    test_motion_position_trajectory();
    test_motion_position_replan_velocity();
    test_motion_inertia_feedforward();
    test_motion_speed_inertia_direct_no_ff();
    test_motion_inertia_feedforward_filter();
    test_motion_speed_limit_app();
    test_motion_torque_ramp_app();
    test_motion_ramp_and_limit();
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
