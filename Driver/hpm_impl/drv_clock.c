/**
 * @file    drv_clock.c
 * @brief   时钟驱动实现（系统时钟树初始化 + cycle 读数与延时）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_clock.h"
#include "hpm_clock_drv.h"
#include "hpm_pllctlv2_drv.h"
#include "hpm_sysctl_drv.h"
#include "hpm_pcfg_drv.h"
#include "hpm_csr_drv.h"

#ifndef ATTR_RAMFUNC
#define ATTR_RAMFUNC __attribute__((section(".fast")))
#endif

/* CPU 频率缓存：intf_clock_init() 完成后锁定（之后时钟树不再改变）。
 * 热路径（25kHz ISR、SPI 期限换算）原先每次走 clock_get_frequency 时钟树查询
 * （flash 内多级函数 + 寄存器读 + 除法），缓存后为一次 RAM 读。 */
static uint32_t s_cpu_freq_hz;

void intf_clock_init(void)
{
    uint32_t cpu0_freq = clock_get_frequency(clock_cpu0);

    if (cpu0_freq == PLLCTL_SOC_PLL_REFCLK_FREQ) {
        pllctlv2_xtal_set_rampup_time(HPM_PLLCTLV2, 32UL * 1000UL * 9U);
        sysctl_clock_set_preset(HPM_SYSCTL, 2);
    }

    pllctlv2_select_reference_clock(HPM_PLLCTLV2, pllctlv2_pll0, 0);
    pllctlv2_select_reference_clock(HPM_PLLCTLV2, pllctlv2_pll1, 0);

    clock_add_to_group(clock_cpu0, 0);
    clock_add_to_group(clock_ahb, 0);
    clock_add_to_group(clock_lmm0, 0);
    clock_add_to_group(clock_mchtmr0, 0);
    clock_add_to_group(clock_rom, 0);
    clock_add_to_group(clock_mot0, 0);
    clock_add_to_group(clock_gpio, 0);
    clock_add_to_group(clock_hdma, 0);
    clock_add_to_group(clock_xpi0, 0);
    clock_add_to_group(clock_adc0, 0);
    clock_add_to_group(clock_adc1, 0);
    clock_add_to_group(clock_can0, 0);
    clock_set_source_divider(clock_can0, clk_src_pll1_clk0, 10);
    /* 板级使用 MCAN3（PA14/PA15） */
    clock_add_to_group(clock_can3, 0);
    clock_set_source_divider(clock_can3, clk_src_pll1_clk0, 10);

    /*
     * 板级使用 SPI1（出轴编码器 PA26-29）/ SPI3（转子编码器 PA10-13）：
     * 节点时钟 PLL1/10 = 80MHz；驱动侧请求 SCLK=10MHz（KTH7823 上限），
     * 由 SPI 内部分频 8 得到（80/((3+1)*2)）。
     */
    clock_add_to_group(clock_spi1, 0);
    clock_set_source_divider(clock_spi1, clk_src_pll1_clk0, 10);
    clock_add_to_group(clock_spi3, 0);
    clock_set_source_divider(clock_spi3, clk_src_pll1_clk0, 10);

    /*
     * GPTMR1（motion 15kHz 拍）必须与 PWM/FOC 同源同倍率，否则两个 ISR 相位缓慢滑移：
     *   PWM  : mot0 = AHB = PLL0CLK0/6 = 160MHz，reload 5333 → 30001.875Hz（FOC/2 = 15000.9375Hz）
     *   默认 : GPTMR1 走复位默认源（OSC24M），reload 1600 → 15000.000Hz
     *   拍频 : 15000.9375 − 15000 = 0.9375Hz（周期 1.067s）→ 每秒级地周期性撞拍
     * 改为 PLL0CLK0/12 = 80MHz 后 reload = 80e6/15000 = 5333 counts = 66.6625µs
     * ＝ 2 × PWM 周期（5333 counts @160MHz），与 FOC 严格 2:1 锁定，拍频消失。
     * 验证：[MOTION] period 应为 31998 cycles（@480MHz）而非 32000。
     */
    clock_add_to_group(clock_gptmr1, 0);
    clock_set_source_divider(clock_gptmr1, clk_src_pll0_clk0, 12);

    clock_connect_group_to_cpu(0, 0);

    /* Bump DCDC to the SDK-recommended voltage for stable 480MHz CPU operation. */
    pcfg_dcdc_set_voltage(HPM_PCFG, 1275);

    /* Configure CPU to 480MHz, AXI/AHB to 160MHz from PLL0CLK0. */
    sysctl_config_cpu0_domain_clock(HPM_SYSCTL, clock_source_pll0_clk0, 2, 3);

    /* Configure PLL0 to 960MHz. */
    pllctlv2_set_postdiv(HPM_PLLCTLV2, pllctlv2_pll0, pllctlv2_clk0, pllctlv2_div_1p0);
    pllctlv2_set_postdiv(HPM_PLLCTLV2, pllctlv2_pll0, pllctlv2_clk1, pllctlv2_div_1p6);
    pllctlv2_set_postdiv(HPM_PLLCTLV2, pllctlv2_pll0, pllctlv2_clk2, pllctlv2_div_2p4);
    pllctlv2_init_pll_with_freq(HPM_PLLCTLV2, pllctlv2_pll0, 960000000);

    clock_update_core_clock();

    clock_set_source_divider(clock_mchtmr0, clk_src_osc24m, 1);

    s_cpu_freq_hz = clock_get_frequency(clock_cpu0);
}

/*
 * 说明：计时统一使用 mcycle (CSR 0xB00, M-mode)，不使用 cycle (0xC00) 与 MCHTMR。
 * 0xC00 (rdcycle) 与 MCHTMR 在本板实测存在异常（进入后跑飞），已由最小化验证移除。
 * mcycle 与 SuperCap 工程 / IrqProfiler 的用法一致。
 */
/**
 * @brief 读取核心 mcycle 计数
 * @return mcycle [cycle]
 */
static inline uint64_t drv_clock_mcycle(void)
{
    return hpm_csr_get_core_mcycle();
}

ATTR_RAMFUNC
uint32_t intf_clock_get_cpu_freq(void)
{
    uint32_t hz = s_cpu_freq_hz;

    return (hz != 0U) ? hz : clock_get_frequency(clock_cpu0); /* init 前回退实时查询 */
}

uint32_t intf_clock_get_ahb_freq(void)
{
    return clock_get_frequency(clock_ahb);
}

uint32_t intf_clock_get_mot0_freq(void)
{
    return clock_get_frequency(clock_mot0);
}

ATTR_RAMFUNC
uint32_t intf_clock_get_cycle(void)
{
    uint32_t value;
    __asm__ volatile("csrr %0, mcycle" : "=r"(value));
    return value;
}

void intf_clock_delay_ms(uint32_t ms)
{
    uint64_t start = drv_clock_mcycle();
    uint64_t ticks = (uint64_t)(hpm_core_clock / 1000U) * (uint64_t)ms;

    while ((drv_clock_mcycle() - start) < ticks) {
    }
}

void intf_clock_delay_us(uint32_t us)
{
    uint64_t start = drv_clock_mcycle();
    uint64_t ticks = (uint64_t)(hpm_core_clock / 1000000U) * (uint64_t)us;

    while ((drv_clock_mcycle() - start) < ticks) {
    }
}
