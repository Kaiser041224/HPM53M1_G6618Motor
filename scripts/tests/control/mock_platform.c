/**
 * @file    mock_platform.c
 * @brief   宿主测试替身：app_foc_current 依赖的参数/逆变桥/时钟/模拟量桩
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_analog_signal.h"
#include "app_hardware_params.h"
#include "app_motor_params.h"
#include "app_software_params.h"

#include <string.h>

static app_hardware_params_t s_hw;
static app_software_params_t s_sw;
static app_motor_params_t s_motor;
static uint32_t s_cycle;

float g_mock_duty[3];
int g_mock_duty_calls;

void mock_params_reset(void) {
    memset(&s_hw, 0, sizeof(s_hw));
    memset(&s_sw, 0, sizeof(s_sw));
    memset(&s_motor, 0, sizeof(s_motor));
    s_cycle = 0U;
    g_mock_duty_calls = 0;

    s_hw.inverter.pwm_freq_hz = 25000U;
    s_hw.inverter.deadtime_ns = 100U;

    s_sw.control.current_loop.kp = 0.3f;
    s_sw.control.current_loop.ki = 100.0f;
    s_sw.control.current_loop.decoupling_en = 0U;
    s_sw.control.limits.i_q_max_a = 10.0f;
    s_sw.control.limits.duty_max = 0.885f;
    s_sw.control.limits.i_trip_a = 10.0f;
    s_sw.control.limits.speed_max_rad_s = 0.0f;

    s_motor.pole_pairs = 10U;
    s_motor.rs_ohm = 0.158f;
    s_motor.ls_h = 118.5e-6f;
    s_motor.flux_linkage_wb = 0.01f;
    s_motor.encoder.electrical_offset_rad = 0.0f;
    s_motor.encoder.direction = 1.0f;
}

void mock_set_current_gains(float kp, float ki) {
    s_sw.control.current_loop.kp = kp;
    s_sw.control.current_loop.ki = ki;
}

void mock_set_i_trip(float a) { s_sw.control.limits.i_trip_a = a; }

const app_hardware_params_t* app_hardware_params_current(void) { return &s_hw; }
const app_software_params_t* app_software_params_current(void) { return &s_sw; }
const app_motor_params_t* app_motor_params_current(void) { return &s_motor; }

int app_3phase_inverter_set_duty_abc(float duty_u, float duty_v, float duty_w) {
    g_mock_duty[0] = duty_u;
    g_mock_duty[1] = duty_v;
    g_mock_duty[2] = duty_w;
    g_mock_duty_calls++;
    return 0;
}

uint32_t intf_clock_get_cycle(void) { return s_cycle; }

bool app_analog_signal_read_all(app_analog_values_t* values) {
    if (values == NULL) {
        return false;
    }
    *values = (app_analog_values_t){0};
    return true;
}
