/** @file mock_gptmr.c
 * @brief GPTMR接口替身
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "intf_gptmr.h"
intf_gptmr_cfg_t g_mock_timer_config[16];
bool g_mock_timer_running[16];
void hpm_gptmr_driver_register(void) {}
int intf_gptmr_init(intf_gptmr_ch_t ch, const intf_gptmr_cfg_t* cfg) {
    g_mock_timer_config[ch] = *cfg;
    g_mock_timer_running[ch] = false;
    return 0;
}
int intf_gptmr_start(intf_gptmr_ch_t ch) {
    extern int g_mock_timer_start_failure;
    if (ch == g_mock_timer_start_failure)
        return -1;
    g_mock_timer_running[ch] = true;
    return 0;
}
int g_mock_timer_start_failure = -1;
int intf_gptmr_stop(intf_gptmr_ch_t ch) {
    g_mock_timer_running[ch] = false;
    return 0;
}
int intf_gptmr_set_frequency(intf_gptmr_ch_t ch, uint32_t hz) {
    g_mock_timer_config[ch].frequency_hz = hz;
    g_mock_timer_running[ch] = true;
    return 0;
}
