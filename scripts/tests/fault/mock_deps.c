/**
 * @file    mock_deps.c
 * @brief   app_fault 主机自测的依赖替身（app_adc / analog / encoder / params / intf_sys）
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_adc.h"
#include "app_analog_signal.h"
#include "app_encoder.h"
#include "app_hardware_params.h"
#include "app_software_params.h"
#include "intf_sys.h"

#include <string.h>

/* ---- app_adc ---- */
static uint16_t s_mock_raw[ADC_CH_COUNT];
static uint32_t s_mock_sequence;
int g_mock_wdog_reenable_calls;
int g_mock_wdog_reenable_last_ch = -1;

void mock_deps_reset(void) {
    memset(s_mock_raw, 0, sizeof(s_mock_raw));
    s_mock_sequence = 0U;
    g_mock_wdog_reenable_calls = 0;
    g_mock_wdog_reenable_last_ch = -1;
}

void mock_set_raw(adc_channel_t ch, uint16_t raw) { s_mock_raw[ch] = raw; }
void mock_set_sequence(uint32_t seq) { s_mock_sequence = seq; }

bool app_adc_get_raw(adc_channel_t ch, uint16_t* raw) {
    if ((ch >= ADC_CH_COUNT) || (raw == NULL)) {
        return false;
    }
    *raw = s_mock_raw[ch];
    return true;
}

uint32_t app_adc_get_sequence(void) { return s_mock_sequence; }

void app_adc_wdog_reenable(adc_channel_t ch) {
    g_mock_wdog_reenable_calls++;
    g_mock_wdog_reenable_last_ch = (int)ch;
}

/* ---- app_analog_signal ---- */
static app_analog_values_t s_mock_values;

void mock_set_analog(const app_analog_values_t* v) { s_mock_values = *v; }

float app_analog_signal_read(adc_channel_t ch) {
    switch (ch) {
    case ADC_CH_I_U: return s_mock_values.i_u_a;
    case ADC_CH_I_V: return s_mock_values.i_v_a;
    case ADC_CH_I_W: return s_mock_values.i_w_a;
    case ADC_CH_V_VBUS: return s_mock_values.v_bus_v;
    default: return 0.0f;
    }
}

bool app_analog_signal_read_all(app_analog_values_t* out) {
    if (out == NULL) {
        return false;
    }
    *out = s_mock_values;
    return true;
}

/* ---- app_analog_signal: 快通道母线替身（NAN = 不可用 → 回退 1kHz 缓存） ---- */
static float s_mock_vbus_fast;
void mock_set_vbus_fast(float v) { s_mock_vbus_fast = v; }
float app_analog_signal_vbus_fast(void) { return s_mock_vbus_fast; }

/* ---- app_encoder ---- */
static uint32_t s_mock_enc_err;
uint32_t app_encoder_get_error_count(app_encoder_id_t id) {
    (void)id;
    return s_mock_enc_err;
}
void mock_enc_set_error_count(uint32_t v) { s_mock_enc_err = v; }

/* ---- 参数单例 ---- */
static app_hardware_params_t s_mock_hardware;
static app_software_params_t s_mock_software;

const app_hardware_params_t* app_hardware_params_current(void) { return &s_mock_hardware; }
const app_software_params_t* app_software_params_current(void) { return &s_mock_software; }
app_hardware_params_t* mock_hardware(void) { return &s_mock_hardware; }
app_software_params_t* mock_software(void) { return &s_mock_software; }

void mock_params_defaults(void) {
    memset(&s_mock_hardware, 0, sizeof(s_mock_hardware));
    memset(&s_mock_software, 0, sizeof(s_mock_software));
    s_mock_hardware.current_sense.a_per_volt = 66.6667f;
    s_mock_hardware.vbus_sense.v_per_volt = 22.2121f;
    s_mock_software.fault.oc_trip_a = 72.9f;
    s_mock_software.fault.vbus_ov_v = 36.0f;
    s_mock_software.fault.vbus_uv_v = 9.0f;
    s_mock_software.fault.slow_debounce = 5U;
    s_mock_software.fault.adc_stall_ms = 10U;
    s_mock_software.fault.enc_err_delta = 3U;
    s_mock_vbus_fast = 24.0f;
    s_mock_values = (app_analog_values_t){0};
    s_mock_values.v_bus_v = 24.0f;
}

/* ---- intf_sys ---- */
uint32_t intf_sys_irq_save(void) { return 0U; }
void intf_sys_irq_restore(uint32_t state) { (void)state; }
