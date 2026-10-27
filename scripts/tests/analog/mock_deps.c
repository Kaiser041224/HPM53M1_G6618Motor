/**
 * @file    mock_deps.c
 * @brief   宿主测试替身：app_analog_signal.c 依赖（app_adc / 硬件参数 / 时钟）
 * @author  Kaiser
 *
 * 仅用于宿主机自测，不参与目标固件。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_adc.h"
#include "app_hardware_params.h"
#include "intf_clock.h"

#include <string.h>

/* ============================================================================
 * app_adc 替身：1kHz 原始码 + 快通道锁存（可分别控制，用于路径隔离断言）
 * ============================================================================ */

static uint16_t s_slow_raw[ADC_CH_COUNT];         /**< app_adc_get_raw（1kHz 路径） */
static uint16_t s_fast_vbus_raw;                  /**< 快通道锁存原始码 */
static uint32_t s_fast_vbus_seq;                  /**< 快通道锁存序号 */
static uint32_t s_fast_vbus_stale;                /**< 快通道陈旧拍数 */
static bool s_fast_vbus_valid;
static uint32_t s_sequence;

void mock_adc_reset(void) {
    memset(s_slow_raw, 0, sizeof(s_slow_raw));
    s_slow_raw[ADC_CH_I_U] = 32768U;
    s_slow_raw[ADC_CH_I_V] = 32768U;
    s_slow_raw[ADC_CH_I_W] = 32768U;
    s_fast_vbus_raw = 0U;
    s_fast_vbus_seq = 0U;
    s_fast_vbus_stale = 0U;
    s_fast_vbus_valid = false;
    s_sequence = 0U;
}

void mock_set_slow_raw(adc_channel_t ch, uint16_t raw) { s_slow_raw[ch] = raw; }

void mock_set_fast_vbus(uint16_t raw, uint32_t sequence) {
    s_fast_vbus_raw = raw;
    s_fast_vbus_seq = sequence;
    s_fast_vbus_valid = true;
}

void mock_set_fast_vbus_valid(bool valid) { s_fast_vbus_valid = valid; }

bool app_adc_get_raw(adc_channel_t ch, uint16_t* raw) {
    if ((ch >= ADC_CH_COUNT) || (raw == NULL)) {
        return false;
    }
    *raw = s_slow_raw[ch];
    return true;
}

bool app_adc_is_valid(void) { return true; }

uint32_t app_adc_get_sequence(void) { return s_sequence; }

bool app_adc_get_vbus_fast(uint16_t* raw) {
    if ((raw == NULL) || !s_fast_vbus_valid) {
        return false;
    }
    *raw = s_fast_vbus_raw;
    return true;
}

uint32_t app_adc_get_vbus_fast_sequence(void) { return s_fast_vbus_seq; }

uint32_t app_adc_get_vbus_fast_stale_count(void) { return s_fast_vbus_stale; }

float app_adc_code_to_volts(uint16_t code) { return ((float)code * 3.3f) / 65535.0f; }

static const app_adc_cfg_t s_mock_adc_cfg = {
    .trigger_delay_ns = 500U,
    .sample_cycle = 10U,
    .resolution = 16U,
};

const app_adc_cfg_t* app_adc_get_config(void) { return &s_mock_adc_cfg; }

void app_adc_register_current_hook(void (*hook)(void)) { (void)hook; }

/* ============================================================================
 * 硬件参数 / 时钟替身
 * ============================================================================ */

static app_hardware_params_t s_mock_hardware;

void mock_hardware_reset(void) {
    memset(&s_mock_hardware, 0, sizeof(s_mock_hardware));
    s_mock_hardware.adc.sample_cycle = 10U;
    s_mock_hardware.adc.trigger_delay_ns = 500U;
    s_mock_hardware.inverter.pwm_freq_hz = 30000U;
    s_mock_hardware.current_sense.a_per_volt = 66.6667f;
    s_mock_hardware.current_sense.bias_v = 1.65f;
    s_mock_hardware.vbus_sense.v_per_volt = 22.2121f;
    s_mock_hardware.ntc.pullup_ohm = 10000U;
    s_mock_hardware.ntc.max_ohm = 1000000.0f;
}

const app_hardware_params_t* app_hardware_params_current(void) { return &s_mock_hardware; }

app_hardware_params_t* app_hardware_params_mutable(void) { return &s_mock_hardware; }

uint32_t intf_clock_get_cycle(void) { return 0U; }

uint32_t intf_clock_get_cpu_freq(void) { return 480000000U; }