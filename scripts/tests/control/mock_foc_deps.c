/**
 * @file    mock_foc_deps.c
 * @brief   宿主测试替身：app_foc.c 编排层依赖（ADC/模拟量/编码器/逆变桥/故障/中断/时钟）
 * @author  Kaiser
 *
 * 编码器替身：每次 app_encoder_sync_sample 序号 +1、机械角 = g_mock_rotor_rad（有效样本）。
 * 电流替身：三相原始码有效，换算结果恒 0 A；母线恒 24 V。
 *
 * 仅用于宿主机自测，不参与目标固件。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_3phase_inverter.h"
#include "app_adc.h"
#include "app_analog_signal.h"
#include "app_encoder.h"
#include "app_fault.h"
#include "app_motor_params.h"
#include "intf_clock.h"
#include "intf_sys.h"

#include "foc_math.h"
#include <string.h>

float g_mock_rotor_rad;      /**< 编码器替身机械角 [rad] */
static uint32_t s_rotor_seq; /**< 编码器替身序号 */

/* ---- ADC / 模拟量 ---- */
void app_adc_register_current_hook(void (*hook)(void)) { (void)hook; }
bool app_adc_is_valid(void) { return true; }
bool app_adc_get_raw(adc_channel_t ch, uint16_t* raw) {
    (void)ch;
    *raw = 32768U;
    return true;
}
float app_analog_signal_convert_raw(adc_channel_t ch, uint16_t raw) {
    (void)ch;
    (void)raw;
    return 0.0f;
}
float app_analog_signal_read(adc_channel_t ch) {
    (void)ch;
    return 24.0f;
}

/* ---- 编码器 ---- */
static void mock_fill_rotor(app_encoder_rotor_snapshot_t* out) {
    memset(out, 0, sizeof(*out));
    out->rad = g_mock_rotor_rad;
    out->seq = s_rotor_seq;
    out->timestamp_cycles = s_rotor_seq * 19200U; /* 40µs @480MHz */
    out->valid = true;
}
int g_mock_rotor_fail;
int app_encoder_sync_sample(uint32_t now_cycles, app_encoder_rotor_snapshot_t* out) {
    (void)now_cycles;
    if (g_mock_rotor_fail) {
        memset(out, 0, sizeof(*out));
        out->valid = false;
        return -1;
    }
    s_rotor_seq++;
    mock_fill_rotor(out);
    return 0;
}
int app_encoder_get_rotor_snapshot(app_encoder_rotor_snapshot_t* out) {
    mock_fill_rotor(out);
    return 0;
}
int app_encoder_get_rotor_raw(uint16_t* raw, bool* valid, uint32_t* seq) {
    *raw = 0U;
    *valid = true;
    if (seq != NULL) {
        *seq = s_rotor_seq;
    }
    return 0;
}

/* ---- 逆变桥 / 故障 / 中断 / 时钟 ---- */
int app_3phase_inverter_enable(void) { return 0; }
void app_3phase_inverter_disable(void) {}
int app_3phase_inverter_release(app_3phase_id_t phase) {
    (void)phase;
    return 0;
}
void app_3phase_inverter_emergency_stop(void) {}
app_fault_state_t app_fault_get_state(void) { return APP_FAULT_STATE_NORMAL; }
uint32_t intf_sys_irq_save(void) { return 0U; }
void intf_sys_irq_restore(uint32_t state) { (void)state; }
uint32_t intf_clock_get_cpu_freq(void) { return 480000000U; }
int g_mock_output_failure;
bool g_mock_output_timestamp_override;
uint32_t g_mock_output_timestamp_cycles;
bool g_mock_output_raw_override;
uint16_t g_mock_output_raw;
int app_encoder_output_sample(app_encoder_rotor_snapshot_t* rotor, uint16_t* output) {
    if (g_mock_output_failure)
        return -1;
    mock_fill_rotor(rotor);
    rotor->seq = ++s_rotor_seq;
    if (g_mock_output_timestamp_override)
        rotor->timestamp_cycles = g_mock_output_timestamp_cycles;
    const app_motor_params_t* motor = app_motor_params_current();
    *output = foc_finite(rotor->rad)
                  ? (uint16_t)(motor->encoder.output_ratio * rotor->rad / 6.283185307f
                               * 65536.0f)
                  : 0U;
    if (g_mock_output_raw_override)
        *output = g_mock_output_raw;
    return 0;
}
