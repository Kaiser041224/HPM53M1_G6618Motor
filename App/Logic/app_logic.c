/*
 * Application Logic - 板级 bring-up：时钟自检 + 各驱动自检 + 25kHz 控制环仿真
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 主循环结构（25kHz 节拍，模拟 FOC 开关频率暂定值）：
 *   1) 每周期：编码器双路采样（更新 Ozone 观测变量 + 计时统计）
 *   2) 1ms 分频：UART / CAN / USB 调试轮询（各自内部 1Hz 打印门限）
 *   3) 1s：RTT 心跳 + 编码器统计汇总
 *   4) 节拍点：mcycle 计时，迟到计数并重同步（心跳轮次不计数，避免打印污染指标）
 *
 * 说明：本文件为 bring-up 测试代码，后续由 FOC 应用替换。
 */

#include <stdbool.h>
#include <stdint.h>

#include "app_debug_inverter.h"
#include "app_debug_can.h"
#include "app_debug_encoder.h"
#include "app_debug_flash.h"
#include "app_debug_motor.h"
#include "app_debug_rtt.h"
#include "app_debug_uart.h"
#include "app_debug_usb.h"
#include "app_gpio.h"
#include "intf_clock.h"
#include "intf_sys.h"

#define APP_LOOP_FREQ_HZ          (25000U) /* 模拟 FOC 开关频率（暂定 25kHz → 40µs） */
#define APP_SLOW_TASK_PERIOD_MS   (1U)     /* UART/CAN/USB 调试轮询分频 */
#define APP_HEARTBEAT_INTERVAL_MS (1000U)

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

    /* 7. 编码器自检（双 KTH7823：SPI3 转子 / SPI1 出轴） */
    app_debug_encoder_init();

    /* 8. Flash 自检（XPI NOR：属性 + 末尾扇区破坏性读写测试） */
    app_debug_flash_init();

    /* 9. 三相半桥输出自检（PWM1：U/V/W 25kHz / 50% 持续输出） */
    app_debug_inverter_init();

    /* 10. 开环旋转自检（V/F，命令 r 启动） */
    app_debug_motor_init();
}

void app_run(void) {
    const uint32_t cpu_freq = intf_clock_get_cpu_freq();
    const uint32_t loop_cycles = cpu_freq / APP_LOOP_FREQ_HZ;
    const uint32_t slow_cycles = (cpu_freq / 1000U) * APP_SLOW_TASK_PERIOD_MS;
    const uint32_t hb_cycles = (cpu_freq / 1000U) * APP_HEARTBEAT_INTERVAL_MS;
    uint32_t next = intf_clock_get_cycle() + loop_cycles;
    uint32_t last_slow = 0U;
    uint32_t last_hb = 0U;
    uint32_t last_printf_cycles = 0U;
    uint32_t heartbeat = 0U;

    for (;;) {
        uint32_t now;
        bool did_heartbeat = false;

        /* 1) 核心：编码器双路采样（25kHz，模拟 FOC 开关频率） */
        app_debug_encoder_sample();

        /* 1b) 开环旋转（V/F）：25kHz 节拍更新三相占空比（未启动时为空操作） */
        app_debug_motor_run_once();

        now = intf_clock_get_cycle();

        /* 2) 低速调试任务（1ms 分频；各自内部有 1Hz 打印门限） */
        if ((uint32_t) (now - last_slow) >= slow_cycles) {
            last_slow = now;
            app_debug_uart_run_once();
            app_debug_can_run_once();
            app_debug_usb_run_once();
        }

        /* 3) 心跳 + 编码器统计汇总（1s） */
        if ((uint32_t) (now - last_hb) >= hb_cycles) {
            uint32_t c0, c1;

            last_hb = now;
            did_heartbeat = true;
            heartbeat++;
            app_gpio_toggle(PIN_LED_STATUS);

            c0 = intf_clock_get_cycle();
            app_debug_printf("hb=%u led=%u printf_cyc=%u\r\n", (unsigned) heartbeat,
                             (unsigned) app_gpio_read(PIN_LED_STATUS),
                             (unsigned) last_printf_cycles);
            c1 = intf_clock_get_cycle();
            last_printf_cycles = c1 - c0;

            app_debug_encoder_print_stats();
        }

        /* 4) 25kHz 节拍：迟到计数并重同步（心跳轮次不计数） */
        now = intf_clock_get_cycle();
        {
            int32_t late = (int32_t) (now - next);

            if (late >= 0) {
                if (!did_heartbeat) {
                    app_debug_encoder_note_loop_late((uint32_t) late);
                }
                next = now + loop_cycles;
            }
        }
        while ((int32_t) (intf_clock_get_cycle() - next) < 0) {
        }
        next += loop_cycles;
    }
}
