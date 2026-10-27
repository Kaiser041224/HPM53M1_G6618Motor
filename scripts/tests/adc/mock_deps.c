/**
 * @file    mock_deps.c
 * @brief   宿主测试替身：app_adc.c 依赖（intf_adc / intf_hrpwm / intf_trgm / intf_gptmr / 硬件参数）
 * @author  Kaiser
 *
 * 设计：所有 intf_* 调用都被"捕获"而非忽略——测试据此断言 PMT 队列形态、SEQ 序列形态与
 * TRGM 连接表。这些形态是 2026-09-19 "首槽污染" 硬件行为的规避契约，必须可回归。
 *
 * 仅用于宿主机自测，不参与目标固件。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_hardware_params.h"
#include "intf_adc.h"
#include "intf_gptmr.h"
#include "intf_hrpwm.h"
#include "intf_trgm.h"

#include <string.h>

/* ============================================================================
 * intf_adc：配置捕获 + PRD 结果替身 + SEQ 帧计数替身
 * ============================================================================ */

intf_adc_cfg_t g_mock_adc_cfg[INTF_ADC_INSTANCE_COUNT];
int g_mock_adc_init_count[INTF_ADC_INSTANCE_COUNT];
uint16_t g_mock_prd_raw[INTF_ADC_INSTANCE_COUNT][16]; /**< 按实例+硬件通道可编程 */
uint32_t g_mock_seq_frames[INTF_ADC_INSTANCE_COUNT];  /**< ADC1 SEQ 帧计数替身 */
uint32_t g_mock_seq_progress[INTF_ADC_INSTANCE_COUNT]; /**< ADC16.SEQ_WR_ADDR 替身 */

void mock_adc_reset(void) {
    memset(g_mock_adc_cfg, 0, sizeof(g_mock_adc_cfg));
    memset(g_mock_adc_init_count, 0, sizeof(g_mock_adc_init_count));
    memset(g_mock_prd_raw, 0, sizeof(g_mock_prd_raw));
    memset(g_mock_seq_frames, 0, sizeof(g_mock_seq_frames));
    memset(g_mock_seq_progress, 0, sizeof(g_mock_seq_progress));
}

int intf_adc_init(intf_adc_ch_t ch, const intf_adc_cfg_t* cfg) {
    uint8_t inst = INTF_ADC_CH_INST(ch);

    if ((inst >= INTF_ADC_INSTANCE_COUNT) || (cfg == NULL)) {
        return -1;
    }
    g_mock_adc_cfg[inst] = *cfg;
    g_mock_adc_init_count[inst]++;
    return 0;
}

int intf_adc_read(intf_adc_ch_t ch, uint16_t* value) {
    uint8_t inst = INTF_ADC_CH_INST(ch);
    uint8_t idx = INTF_ADC_CH_IDX(ch);

    if ((inst >= INTF_ADC_INSTANCE_COUNT) || (value == NULL)) {
        return -1;
    }
    *value = g_mock_prd_raw[inst][idx];
    return 0;
}

int intf_adc_get_prd_result(uint8_t inst, uint8_t hw_ch, uint16_t* raw) {
    if ((inst >= INTF_ADC_INSTANCE_COUNT) || (hw_ch >= 16U) || (raw == NULL)) {
        return -1;
    }
    *raw = g_mock_prd_raw[inst][hw_ch];
    return 0;
}

int intf_adc_get_seq_frames(uint8_t inst, uint32_t* frames) {
    if ((inst >= INTF_ADC_INSTANCE_COUNT) || (frames == NULL)) {
        return -1;
    }
    *frames = g_mock_seq_frames[inst];
    return 0;
}

int g_mock_clear_status_calls;
int intf_adc_clear_status(uint8_t inst) {
    if (inst >= INTF_ADC_INSTANCE_COUNT) {
        return -1;
    }
    g_mock_clear_status_calls++;
    return 0;
}

int intf_adc_get_seq_progress(uint8_t inst, uint32_t* wr_addr) {
    if ((inst >= INTF_ADC_INSTANCE_COUNT) || (wr_addr == NULL)) {
        return -1;
    }
    *wr_addr = g_mock_seq_progress[inst];
    return 0;
}

int intf_adc_read_voltage(intf_adc_ch_t ch, float* voltage_mv) {
    (void)ch;
    (void)voltage_mv;
    return -1;
}

int intf_adc_start(intf_adc_ch_t ch) {
    (void)ch;
    return 0;
}

int intf_adc_stop(intf_adc_ch_t ch) {
    (void)ch;
    return 0;
}

int intf_adc_get_diag_snapshot(intf_adc_diag_snapshot_t* snapshot) {
    (void)snapshot;
    return -1;
}

void intf_adc_reset_diag_max(void) {}

void intf_adc_wdog_reenable(intf_adc_ch_t ch) { (void)ch; }

/* ============================================================================
 * intf_trgm：连接表捕获
 * ============================================================================ */

#define MOCK_TRGM_MAX (16)

typedef struct {
    intf_trgm_src_t src;
    intf_trgm_dst_t dst;
} mock_trgm_edge_t;

mock_trgm_edge_t g_mock_trgm[MOCK_TRGM_MAX];
int g_mock_trgm_count;

void mock_trgm_reset(void) {
    memset(g_mock_trgm, 0, sizeof(g_mock_trgm));
    g_mock_trgm_count = 0;
}

int intf_trgm_connect(intf_trgm_src_t src, intf_trgm_dst_t dst) {
    if (g_mock_trgm_count < MOCK_TRGM_MAX) {
        g_mock_trgm[g_mock_trgm_count].src = src;
        g_mock_trgm[g_mock_trgm_count].dst = dst;
        g_mock_trgm_count++;
    }
    return 0;
}

/** @brief 连接表里是否存在某条边 */
bool mock_trgm_has(intf_trgm_src_t src, intf_trgm_dst_t dst) {
    for (int i = 0; i < g_mock_trgm_count; i++) {
        if ((g_mock_trgm[i].src == src) && (g_mock_trgm[i].dst == dst)) {
            return true;
        }
    }
    return false;
}

/* ============================================================================
 * intf_hrpwm / intf_gptmr：调用捕获
 * ============================================================================ */

uint32_t g_mock_trigger_delay_ns;
uint8_t g_mock_trigger_cmp_index;
int g_mock_trigger_cmp_calls;
int g_mock_counter_only_calls;
int g_mock_gptmr_init_calls;
int g_mock_gptmr_start_calls;

void mock_hrpwm_reset(void) {
    g_mock_trigger_delay_ns = 0U;
    g_mock_trigger_cmp_index = 0xFFU;
    g_mock_trigger_cmp_calls = 0;
    g_mock_counter_only_calls = 0;
    g_mock_gptmr_init_calls = 0;
    g_mock_gptmr_start_calls = 0;
}

int intf_hrpwm_config_trigger_cmp(intf_hrpwm_inst_t inst, uint8_t cmp_index, uint32_t delay_ns) {
    (void)inst;
    g_mock_trigger_cmp_index = cmp_index;
    g_mock_trigger_delay_ns = delay_ns;
    g_mock_trigger_cmp_calls++;
    return 0;
}

int intf_hrpwm_set_trigger_cmp_delay(intf_hrpwm_inst_t inst, uint8_t cmp_index, uint32_t delay_ns) {
    (void)inst;
    (void)cmp_index;
    g_mock_trigger_delay_ns = delay_ns;
    return 0;
}

int intf_hrpwm_start_counter_only(intf_hrpwm_inst_t inst) {
    (void)inst;
    g_mock_counter_only_calls++;
    return 0;
}

int intf_gptmr_init(intf_gptmr_ch_t ch, const intf_gptmr_cfg_t* cfg) {
    (void)ch;
    (void)cfg;
    g_mock_gptmr_init_calls++;
    return 0;
}

int intf_gptmr_start(intf_gptmr_ch_t ch) {
    (void)ch;
    g_mock_gptmr_start_calls++;
    return 0;
}

/* ============================================================================
 * 硬件参数单例
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

/* ============================================================================
 * 驱动注册（app_adc.c 的 extern 声明）
 * ============================================================================ */

void hpm_adc_driver_register(void) {}
void hpm_trgm_driver_register(void) {}
void hpm_hrpwm_driver_register(void) {}
void hpm_gptmr_driver_register(void) {}