/**
 * @file    app_debug_adc.c
 * @brief   采样链调试实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_debug_adc.h"

#include "app_adc.h"
#include "app_analog_signal.h"
#include "app_debug_rtt.h"
#include "intf_adc.h"
#include "intf_clock.h"

#include <stdbool.h>
#include <stddef.h>

/* ============================================================================
 * Ozone 观测变量（.noncacheable.bss：启动清零 + 调试器直读，不受 D-Cache 影响）
 * 每个 25kHz 控制周期由 app_debug_adc_update() 刷新
 * ============================================================================ */

volatile float g_adc_i_u_a __attribute__((section(".noncacheable.bss")));
volatile float g_adc_i_v_a __attribute__((section(".noncacheable.bss")));
volatile float g_adc_i_w_a __attribute__((section(".noncacheable.bss")));
volatile float g_adc_v_bus_v __attribute__((section(".noncacheable.bss")));
volatile float g_adc_r_ntc0_ohm __attribute__((section(".noncacheable.bss")));
volatile float g_adc_r_ntc1_ohm __attribute__((section(".noncacheable.bss")));
volatile uint16_t g_adc_raw[ADC_CH_COUNT] __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_adc_sequence __attribute__((section(".noncacheable.bss")));

void app_debug_adc_update(void) {
    app_analog_values_t values;

    if (app_analog_signal_read_all(&values)) {
        g_adc_i_u_a = values.i_u_a;
        g_adc_i_v_a = values.i_v_a;
        g_adc_i_w_a = values.i_w_a;
        g_adc_v_bus_v = values.v_bus_v;
        g_adc_r_ntc0_ohm = values.r_ntc0_ohm;
        g_adc_r_ntc1_ohm = values.r_ntc1_ohm;
    }

    for (uint8_t ch = 0U; ch < (uint8_t)ADC_CH_COUNT; ch++) {
        uint16_t raw;

        if (app_adc_get_raw((adc_channel_t)ch, &raw)) {
            g_adc_raw[ch] = raw;
        }
    }
    g_adc_sequence = app_adc_get_sequence();
}

/* 通道名/单位（与 adc_channel_t 顺序一致） */
static const char* const s_ch_names[ADC_CH_COUNT] = {
    "I_U", "I_V", "I_W", "V_VBUS", "NTC0", "NTC1", "V_CANID",
};
static const char* const s_ch_units[ADC_CH_COUNT] = {
    "A", "A", "A", "V", "ohm", "ohm", "V",
};

void app_debug_adc_init(void) {
    const app_adc_cfg_t* cfg = app_adc_get_config();

    app_debug_printf(
        "[ADC] chain: PWM1 CMP10 +%u ns -> TRGM PTRGI0A -> ADC0[IN2,IN3,IN4] PMT | "
        "ADC1[IN15,IN6,IN11,IN1] SEQ 1kHz(GPTMR0)\r\n",
        (unsigned)cfg->trigger_delay_ns);
    app_debug_printf(
        "[ADC] %u bit / sample_cycle=%u / 触发 = PWM1 计数（25kHz）\r\n", (unsigned)cfg->resolution,
        (unsigned)cfg->sample_cycle);
}

void app_debug_adc_dump_channels(void) {
    const app_adc_cfg_t* cfg = app_adc_get_config();

    app_debug_printf(
        "[ADC] seq=%u valid=%u delay=%u ns\r\n", (unsigned)app_adc_get_sequence(),
        (unsigned)(app_adc_is_valid() ? 1U : 0U), (unsigned)cfg->trigger_delay_ns);

    for (uint8_t ch = 0U; ch < (uint8_t)ADC_CH_COUNT; ch++) {
        uint16_t raw = 0U;
        bool ok = app_adc_get_raw((adc_channel_t)ch, &raw);
        float phys = app_analog_signal_read((adc_channel_t)ch);

        if (ok) {
            app_debug_printf(
                "[ADC] %-6s raw=%-5u %8.1f mV  %9.4f %s\r\n", s_ch_names[ch], (unsigned)raw,
                (double)(app_adc_code_to_volts(raw) * 1000.0f), (double)phys, s_ch_units[ch]);
        } else {
            app_debug_printf("[ADC] %-6s no data\r\n", s_ch_names[ch]);
        }
    }
}

void app_debug_adc_dump_diag(void) {
    static intf_adc_diag_snapshot_t s_last;
    static uint32_t s_last_cycle;
    static bool s_initialized;

    intf_adc_diag_snapshot_t now;
    uint32_t cycles = intf_clock_get_cycle();
    uint32_t dt_cycles = s_initialized ? (cycles - s_last_cycle) : 0U;

    if (intf_adc_get_diag_snapshot(&now) != 0) {
        app_debug_printf("[ADC] diag unavailable\r\n");
        return;
    }

    app_debug_printf(
        "[ADC] seq=%u | dt=%u cyc\r\n", (unsigned)app_adc_get_sequence(), (unsigned)dt_cycles);

    for (uint8_t inst = 0U; inst < (uint8_t)INTF_ADC_INSTANCE_COUNT; inst++) {
        uint32_t rate = 0U;
        uint32_t isr_permille = 0U;

        if (dt_cycles > 0U) {
            uint32_t delta = now.pmt_complete[inst] - s_last.pmt_complete[inst];
            uint64_t delta_isr = now.isr_total_cycles[inst] - s_last.isr_total_cycles[inst];

            rate = (uint32_t)(((uint64_t)delta * (uint64_t)intf_clock_get_cpu_freq()) / dt_cycles);
            isr_permille = (uint32_t)((delta_isr * 1000U) / dt_cycles);
        }

        app_debug_printf(
            "[ADC] ADC%u: irq=%u pmt=%u cb=%u drop=%u inv=%u(cyc=%u trg=%u ch=%u) fallback=%u "
            "isr_max=%u cyc isr_cpu=%u.%u%% rate=%u Hz\r\n",
            (unsigned)inst, (unsigned)now.irq_entry[inst], (unsigned)now.pmt_complete[inst],
            (unsigned)now.pmt_callback[inst], (unsigned)now.pmt_startup_drop[inst],
            (unsigned)now.pmt_invalid[inst], (unsigned)now.pmt_invalid_cycle[inst],
            (unsigned)now.pmt_invalid_trig[inst], (unsigned)now.pmt_invalid_channel[inst],
            (unsigned)now.pmt_cycle_fallback[inst], (unsigned)now.isr_cycles_max[inst],
            (unsigned)(isr_permille / 10U), (unsigned)(isr_permille % 10U), (unsigned)rate);
    }

    /* 关键寄存器状态（配置核对） */
    for (uint8_t inst = 0U; inst < (uint8_t)INTF_ADC_INSTANCE_COUNT; inst++) {
        app_debug_printf(
            "[ADC] ADC%u regs: CONV_CFG1=0x%08X ADC_CFG0=0x%08X BUF_CFG0=0x%08X CONFIG0=0x%08X\r\n",
            (unsigned)inst, (unsigned)now.reg_conv_cfg1[inst], (unsigned)now.reg_adc_cfg0[inst],
            (unsigned)now.reg_buf_cfg0[inst], (unsigned)now.reg_config0[inst]);
    }

    /* ADC1 慢通道：SEQ 运行状态与 PRD_RESULT（序列结果寄存器，手册 §53.2.4） */
    {
        const uint8_t inst = 1U;

        app_debug_printf(
            "[ADC] ADC1 sts: INT_STS=0x%08X SEQ_CFG0=0x%08X\r\n", (unsigned)now.reg_int_sts[inst],
            (unsigned)now.reg_seq_cfg0[inst]);
        app_debug_printf(
            "[ADC] ADC1 prd: ch6=%u ch11=%u ch1=%u ch15=%u\r\n",
            (unsigned)(now.reg_prd_result[inst][6] & 0xFFFFU),
            (unsigned)(now.reg_prd_result[inst][11] & 0xFFFFU),
            (unsigned)(now.reg_prd_result[inst][1] & 0xFFFFU),
            (unsigned)(now.reg_prd_result[inst][15] & 0xFFFFU));
    }

    /* 最近一帧 PMT 原始字（仅 ADC0 为 PMT 模式）：bit31=cycle, [28:25]=trig, [24:20]=ch,
     * [15:0]=result */
    {
        const uint8_t inst = 0U;

        app_debug_printf("[ADC] ADC%u raw:", (unsigned)inst);
        for (uint8_t i = 0U; i < 4U; i++) {
            uint32_t raw_word = now.pmt_last[inst][i];

            app_debug_printf(
                " [cyc=%u trg=%u ch=%u seq=%u r=%u]", (unsigned)((raw_word >> 31) & 1U),
                (unsigned)((raw_word >> 25) & 0xFU), (unsigned)((raw_word >> 20) & 0x1FU),
                (unsigned)((raw_word >> 29) & 0x3U), (unsigned)(raw_word & 0xFFFFU));
        }
        app_debug_printf("\r\n");
    }

    s_last = now;
    s_last_cycle = cycles;
    s_initialized = true;
}
