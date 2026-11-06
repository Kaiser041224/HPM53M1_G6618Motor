/**
 * @file    mock_deps.c
 * @brief   USB 协议主机自测：app_usb_proto.c 的外部依赖替身
 * @author  Kaiser
 *
 * 仅用于宿主机自测，不参与目标固件。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mock_deps.h"

#include "app_debug_rtt.h"
#include "app_fault.h"
#include "app_usb.h"
#include "intf_clock.h"

#include <string.h>

mock_state_t g_mock;

void mock_reset(void) {
    memset(&g_mock, 0, sizeof(g_mock));

    g_mock.ready = true;
    g_mock.write_rc = 0;
    g_mock.analog_ok = true;
    g_mock.ntc_valid = true;
    g_mock.foc_state = APP_FOC_STATE_OFF;
    g_mock.foc_snap.voltage_utilization = 0.5f;
    g_mock.foc_snap.omega_e_rad_s = 62.831853f;
    g_mock.foc_snap.i_d_a = 0.25f;
    g_mock.foc_snap.i_q_a = -0.5f;
    g_mock.foc_snap.speed_out_rad_s = 1.5f;
    g_mock.motion_snap.torque_nm = 2.0f;
    g_mock.motion_snap.shaft_rad = 0.75f;
    g_mock.motion_snap.mode = APP_MOTION_MODE_TORQUE;
    g_mock.analog.i_u_a = 1.0f;
    g_mock.analog.i_v_a = -2.0f;
    g_mock.analog.i_w_a = 1.0f;
    g_mock.analog.v_bus_v = 24.0f;
    g_mock.analog.r_ntc0_ohm = 100000.0f;
    g_mock.analog.r_ntc1_ohm = 90000.0f;
    g_mock.ntc0_c = 25.0f;
    g_mock.ntc1_c = 30.0f;
    g_mock.enc_pair.valid = true;
    g_mock.enc_pair.raw = 1234U;
    g_mock.enc_pair.rad = 0.5f;
    g_mock.enc_output_raw = 5678U;
    g_mock.rotor_deg = 90.0f;
    g_mock.output_deg = 270.0f;
}

/* ---- bulk 通道 ---- */

int app_usb_bulk_write(const uint8_t *data, size_t len, uint32_t timeout_ms) {
    (void) timeout_ms;

    if (g_mock.write_rc != 0) {
        return g_mock.write_rc;
    }
    if ((data == NULL) || (len > sizeof(g_mock.tx))) {
        return -1;
    }
    memcpy(g_mock.tx, data, len);
    g_mock.tx_len = (uint16_t) len;
    g_mock.tx_count++;
    return 0;
}

int app_usb_bulk_read(uint8_t *data, size_t len) {
    size_t remain = (size_t) (g_mock.rx_len - g_mock.rx_pos);
    size_t count = (remain < len) ? remain : len;

    if (count > 0U) {
        memcpy(data, &g_mock.rx[g_mock.rx_pos], count);
        g_mock.rx_pos = (uint16_t) (g_mock.rx_pos + count);
    }
    return (int) count;
}

bool app_usb_bulk_ready(void) { return g_mock.ready; }

/* ---- 模拟量 / 编码器 ---- */

bool app_analog_signal_read_all(app_analog_values_t *values) {
    *values = g_mock.analog;
    return g_mock.analog_ok;
}

bool app_analog_signal_ntc_temp_c(adc_channel_t ch, float *temp_c) {
    if (!g_mock.ntc_valid) {
        return false;
    }
    *temp_c = (ch == ADC_CH_NTC0) ? g_mock.ntc0_c : g_mock.ntc1_c;
    return true;
}

int app_encoder_output_sample(app_encoder_rotor_snapshot_t *rotor, uint16_t *output) {
    *rotor = g_mock.enc_pair;
    *output = g_mock.enc_output_raw;
    return g_mock.enc_pair.valid ? 0 : -1;
}

int app_encoder_read_deg(app_encoder_id_t id, float *deg) {
    *deg = (id == APP_ENCODER_ROTOR) ? g_mock.rotor_deg : g_mock.output_deg;
    return 0;
}

/* ---- 故障 ---- */

uint32_t app_fault_get_codes(void) { return g_mock.fault_codes; }

uint32_t app_fault_get_latched(void) { return g_mock.fault_latched; }

int app_fault_clear(void) {
    if (g_mock.fault_clear_rc != 0) {
        return g_mock.fault_clear_rc;
    }
    g_mock.fault_codes = 0U;
    g_mock.fault_latched = 0U;
    return 0;
}

/* ---- FOC ---- */

app_foc_state_t app_foc_get_state(void) { return g_mock.foc_state; }

int app_foc_enable(void) {
    if (g_mock.foc_enable_rc == 0) {
        g_mock.foc_state = APP_FOC_STATE_READY;
    }
    return g_mock.foc_enable_rc;
}

int app_foc_request_stop(void) { return g_mock.stop_rc; }

bool app_foc_stop_pending(void) { return g_mock.stop_pending; }

void app_foc_current_get_snapshot(app_foc_current_snapshot_t *out) { *out = g_mock.foc_snap; }

/* ---- motion ---- */

void app_motion_get_snapshot(app_motion_snapshot_t *out) { *out = g_mock.motion_snap; }

int app_motion_torque(float torque) {
    g_mock.motion_call = 1;
    g_mock.motion_arg = torque;
    return g_mock.motion_rc;
}

int app_motion_speed(float speed) {
    g_mock.motion_call = 2;
    g_mock.motion_arg = speed;
    return g_mock.motion_rc;
}

int app_motion_position(float position_rad, float vel_ff_rad_s, float torque_ff_nm) {
    (void) vel_ff_rad_s;
    (void) torque_ff_nm;
    g_mock.motion_call = 3;
    g_mock.motion_arg = position_rad;
    return g_mock.motion_rc;
}

int app_motion_release_output(void) {
    g_mock.release_count++;
    return 0;
}

/* ---- 时钟 / 日志 ---- */

uint32_t intf_clock_get_cycle(void) { return g_mock.cycles; }

uint32_t intf_clock_get_cpu_freq(void) { return 480000000U; }

int app_debug_printf(const char *fmt, ...) {
    (void) fmt;
    return 0;
}
