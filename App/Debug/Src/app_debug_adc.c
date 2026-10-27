/**
 * @file    app_debug_adc.c
 * @brief   采样链调试实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_debug_adc.h"

#include "app_hardware_params.h"
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
        "[ADC] %u bit / sample_cycle=%u / 触发 = PWM1 计数（30kHz）\r\n", (unsigned)cfg->resolution,
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
    static uint32_t s_last_seq_frames;
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

    /* dt 的自校验：intf_clock_get_cycle() 为 **32 位** cycle 计数，480MHz 下每
     * 2^32/480e6 ≈ 8.94s 回绕一次。两次 dump 间隔超过一个回绕期时 32 位相减会
     * 少算整数倍周期 → `rate`/`isr_cpu` 出现不可能值（实测 766 kHz、551.3%）。
     * 判据：占用率 >100% 不可能；ADC0 的 PMT 速率必须 ≈ pwm_freq（±20%）。
     * 任一不成立即判本窗无效并显式标 n/a，避免误导排查。 */
    const uint32_t pwm_hz = app_hardware_params_current()->inverter.pwm_freq_hz;
    bool window_ok = (dt_cycles > 0U);

    for (uint8_t inst = 0U; inst < (uint8_t)INTF_ADC_INSTANCE_COUNT; inst++) {
        uint32_t rate = 0U;
        uint32_t isr_permille = 0U;
        bool inst_ok = window_ok;

        if (window_ok) {
            uint32_t delta = now.pmt_complete[inst] - s_last.pmt_complete[inst];
            uint64_t delta_isr = now.isr_total_cycles[inst] - s_last.isr_total_cycles[inst];

            rate = (uint32_t)(((uint64_t)delta * (uint64_t)intf_clock_get_cpu_freq()) / dt_cycles);
            isr_permille = (uint32_t)((delta_isr * 1000U) / dt_cycles);
            if (isr_permille > 1000U) {
                inst_ok = false;
            }
            if ((inst == 0U) && (pwm_hz > 0U) && (dt_cycles > 1000000U)) {
                uint32_t dev_milli = (uint32_t)(((uint64_t)rate * 1000U) / pwm_hz);

                if ((dev_milli > 1200U) || (dev_milli < 800U)) {
                    inst_ok = false;
                }
            }
        }

        app_debug_printf(
            "[ADC] ADC%u: irq=%u pmt=%u cb=%u drop=%u inv=%u(cyc=%u trg=%u ch=%u) fallback=%u "
            "isr_max=%u cyc ",
            (unsigned)inst, (unsigned)now.irq_entry[inst], (unsigned)now.pmt_complete[inst],
            (unsigned)now.pmt_callback[inst], (unsigned)now.pmt_startup_drop[inst],
            (unsigned)now.pmt_invalid[inst], (unsigned)now.pmt_invalid_cycle[inst],
            (unsigned)now.pmt_invalid_trig[inst], (unsigned)now.pmt_invalid_channel[inst],
            (unsigned)now.pmt_cycle_fallback[inst], (unsigned)now.isr_cycles_max[inst]);
        if (inst_ok) {
            app_debug_printf(
                "isr_cpu=%u.%u%% rate=%u Hz\r\n", (unsigned)(isr_permille / 10U),
                (unsigned)(isr_permille % 10U), (unsigned)rate);
        } else {
            if (dt_cycles == 0U) {
                app_debug_printf("isr_cpu=n/a rate=n/a  [首次无基线: 再跑一次 adc diag]\r\n");
            } else {
                app_debug_printf(
                    "isr_cpu=n/a rate=n/a  [本窗无效: 两次间隔 >8.9s 时 32 位 cycle 计数"
                    "回绕, 或基线变化; 请连续两次 adc diag 取第二次]\r\n");
            }
        }
    }

    /* ADC1 SEQ 速率与快通道母线活性（2026-10-26：vbus 与三相电流同源触发，
     * 故 SEQ 速率应 ≈ pwm_freq；stale 增长 = ADC1 链未产出新帧 → 电流环回退 1kHz） */
    {
        uint32_t seq_frames = 0U;
        uint32_t seq_rate = 0U;
        uint32_t seq_wr = 0U;
        uint16_t vbus_raw = 0U;
        bool vbus_have = app_adc_get_vbus_fast(&vbus_raw);

        {
            uint32_t wr = 0U;

            /* SEQ 硬件写指针：ADC1 无周期中断后，这是"SEQ 仍在写入"的唯一证据，
             * 应当在每次 dump 之间推进（不推进 → vbus 快通道会回退 1kHz）。 */
            (void)intf_adc_get_seq_progress(1U, &wr);
            seq_wr = wr;
        }
        if (intf_adc_get_seq_frames(1U, &seq_frames) == 0) {
            if (dt_cycles > 0U) {
                uint32_t delta = seq_frames - s_last_seq_frames;

                seq_rate = (uint32_t)(((uint64_t)delta * (uint64_t)intf_clock_get_cpu_freq())
                                      / dt_cycles);
            }
            s_last_seq_frames = seq_frames;
        }
        app_debug_printf(
            "[ADC] ADC1 SEQ: frames=%u rate=%u Hz (expect ~pwm_freq) | "
            "seq_wr=%u (ADC1 无中断且非 DMA → frames/seq_wr 恒 0 = 预期, 非故障) | "
            "vbus_fast: valid=%u raw=%u seq=%u stale=%u\r\n",
            (unsigned)seq_frames, (unsigned)seq_rate, (unsigned)seq_wr,
            (unsigned)(vbus_have ? 1U : 0U),
            (unsigned)vbus_raw, (unsigned)app_adc_get_vbus_fast_sequence(),
            (unsigned)app_adc_get_vbus_fast_stale_count());
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
