/*
 * Application Logic - 板级 bring-up：时钟自检 + RTT 心跳
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 诊断目标：
 *   1) 实测 CPU 频率（MCHTMR 独立时基），校验 PLL 是否真正生效
 *   2) RTT 心跳：printf 耗时周期数 + delay 实际周期数
 *   3) PB01 引脚回读（LED；注：HPM53M1 上 PB01 为模拟端口，无法驱动 LED）
 * 说明：本文件为 bring-up 测试代码，后续由 FOC 应用替换。
 */

#include <stdint.h>

#include "app_debug_can.h"
#include "app_debug_rtt.h"
#include "app_debug_uart.h"
#include "app_debug_usb.h"
#include "app_gpio.h"
#include "intf_clock.h"
#include "intf_sys.h"

#define APP_LED_BLINK_INTERVAL_MS (500U)

void app_init(void) {
    /*
     * 0. 复位诊断：
     *    - s_boot_seq 位于 NOLOAD 段（复位不清零）：若逐次递增 = 芯片在复位循环；
     *      若恒定 = 非复位（上位机重读缓冲）。
     *    - rst_status 为 PPOR.RESET_STATUS（只读）：bit0=欠压、bit4=调试复位、
     *      bit16/17=看门狗、bit24=PMIC 看门狗、bit31=软件复位。
     */
    static volatile uint32_t s_boot_seq __attribute__((section(".noncacheable")));
    uint32_t rst_status;

    s_boot_seq++;
    rst_status = intf_sys_get_reset_status();
    app_debug_printf(
        "boot: seq=%u rst_status=0x%08x\r\n", (unsigned)s_boot_seq, (unsigned)rst_status);

    /* 1. 系统时钟（顺序与 SuperCap 一致：board_init -> intf_clock_init）
     *    CPU 480MHz / AXI-AHB 160MHz / PLL0 960MHz / DCDC 1275mV */
    intf_clock_init();

    /* 2. GPIO 驱动注册 + PA09（DRV_+12V_EN，默认输出低=关闭） */
    app_gpio_init();

    /* 3. 频率自检（寄存器读数；实测频率校验因 MCHTMR/cycle 异常暂缓） */
    app_debug_printf(
        "clock: cpu=%u Hz, ahb=%u Hz\r\n", (unsigned)intf_clock_get_cpu_freq(),
        (unsigned)intf_clock_get_ahb_freq());

    /* 4. UART0 自检（PA00/PA01，115200 8N1）：TX 周期输出 + RX 回显 */
    app_debug_uart_init();

    /* 5. CAN 自检（MCAN3，经典 CAN @1Mbps，TX ID=0x114） */
    app_debug_can_init();

    /* 6. USB 自检（USB0 CDC 虚拟串口，J10） */
    app_debug_usb_init();
}

void app_run(void) {
    static uint32_t heartbeat;
    static uint32_t last_printf_cycles;
    static uint32_t last_delay_cycles;
    uint32_t c0, c1;

    heartbeat++;
    app_gpio_toggle(PIN_LED_STATUS);

    c0 = intf_clock_get_cycle();
    app_debug_printf(
        "hb=%u led=%u printf_cyc=%u delay_cyc=%u\r\n", (unsigned)heartbeat,
        (unsigned)app_gpio_read(PIN_LED_STATUS), (unsigned)last_printf_cycles,
        (unsigned)last_delay_cycles);
    c1 = intf_clock_get_cycle();
    last_printf_cycles = c1 - c0;

    /* UART 自检：RX 回显 + 周期 TX（UART0，115200） */
    app_debug_uart_run_once();

    /* CAN 自检：周期 TX（0x114）+ RX 分发 + 状态 */
    app_debug_can_run_once();

    /* USB 自检：RX 回显 + 周期 TX + DTR 上报 */
    app_debug_usb_run_once();

    intf_clock_delay_ms(APP_LED_BLINK_INTERVAL_MS);
    last_delay_cycles = intf_clock_get_cycle() - c1;
}
