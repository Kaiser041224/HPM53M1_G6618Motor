/**
 * @file    app_logic.c
 * @brief   板级 bring-up：时钟自检 + 各驱动自检 + 控制环仿真（节拍 = inverter.pwm_freq_hz）
 * @author  Kaiser
 *
 * 主循环结构（节拍 = config/hardware.yaml 的 inverter.pwm_freq_hz，即半桥开关频率/FOC 闭环频率）：
 *   1) 每周期：编码器双路采样（更新 Ozone 观测变量 + 计时统计）
 *   2) 1ms 分频：UART / CAN / USB 调试轮询（各自内部 1Hz 打印门限）
 *   3) 1s：RTT 心跳 + 编码器统计汇总
 *   4) 节拍点：mcycle 计时，迟到计数并重同步（心跳轮次不计数，避免打印污染指标）
 *
 * 说明：本文件为 bring-up 测试代码，后续由 FOC 应用替换。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_adc.h"
#include "app_analog_signal.h"
#include "app_debug_adc.h"
#include "app_debug_can.h"
#include "app_debug_encoder.h"
#include "app_debug_flash.h"
#include "app_debug_inverter.h"
#include "app_debug_motor.h"
#include "app_debug_rtt.h"
#include "app_debug_uart.h"
#include "app_fault.h"
#include "app_foc.h"
#include "app_gpio.h"
#include "app_hardware_params.h"
#include "app_motor_params.h"
#include "app_terminal.h"
#include "app_software_params.h"
#include "app_usb.h"
#include "intf_clock.h"
#include "intf_sys.h"

#define APP_SLOW_TASK_PERIOD_MS   (1U) /* UART/CAN/USB 调试轮询分频 */
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

    /* 0b. 参数单例初始化 + 摘要（验证 YAML 参数管线端到端：
     * config/{motor,hardware,software}.yaml → 生成 → 加载） */
    {
        app_motor_params_init();
        app_hardware_params_init();
        app_software_params_init();

        const app_motor_params_t *motor_params = app_motor_params_current();
        const app_hardware_params_t *hardware = app_hardware_params_current();
        const app_software_params_t *software = app_software_params_current();

        app_debug_printf(
            "params: pp=%u rs=%.4f ls=%g | a/v=%.4f vbus/v=%.4f | oc=%.1f ov=%.1f uv=%.1f | "
            "pwm=%u/%u\r\n",
            (unsigned)motor_params->pole_pairs, (double)motor_params->rs_ohm,
            (double)motor_params->ls_h, (double)hardware->current_sense.a_per_volt,
            (double)hardware->vbus_sense.v_per_volt, (double)software->fault.oc_trip_a,
            (double)software->fault.vbus_ov_v, (double)software->fault.vbus_uv_v,
            (unsigned)hardware->inverter.pwm_freq_hz, (unsigned)hardware->inverter.deadtime_ns);
    }

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

    /* 5. CAN 自检（MCAN3，经典 CAN；总线波特率与周期帧 ID 来源 config/software.yaml） */
    app_debug_can_init();

    /* 6. USB 终端（USB0 CDC 虚拟串口，J10；CherrySH 交互 Terminal，1kHz 慢任务轮询） */
    app_usb_init();
    app_terminal_init();

    /* 7. 编码器自检（双 KTH7823：SPI3 转子 / SPI1 出轴） */
    app_debug_encoder_init();

    /* 8. Flash 自检（XPI NOR：属性 + 末尾扇区破坏性读写测试） */
    app_debug_flash_init();

    /* 9. 三相半桥输出自检（PWM1：U/V/W 默认频率 / 50% 持续输出；频率见 config/hardware.yaml） */
    app_debug_inverter_init();

    /* 10. 故障保护（在 ADC 之前：提供 WDOG 阈值与回调） */
    app_fault_init(NULL);

    /* 11. ADC 采样链（PWM1 CMP10 触发 → TRGM → 双 ADC PMT：三相电流 + 母线/NTC；
     *     ADC0 电流通道启用 WDOG；ADC1 序列完成回调接入故障 tick @1kHz）
     *     内部启动 PWM1 计数（仅计数、输出仍由逆变桥控制） */
    {
        app_adc_cfg_t adc_cfg;
        uint16_t wdog_hi;
        uint16_t wdog_lo;

        memset(&adc_cfg, 0, sizeof(adc_cfg));
        app_fault_get_wdog_raw(&wdog_hi, &wdog_lo);
        adc_cfg.wdog_en = true;
        adc_cfg.wdog_thshd_high = wdog_hi;
        adc_cfg.wdog_thshd_low = wdog_lo;
        adc_cfg.wdog_cb = app_fault_on_wdog;
        adc_cfg.wdog_cb_user = NULL;
        adc_cfg.slow_cb = app_fault_tick;
        app_adc_init(&adc_cfg);
    }
    app_analog_signal_init();
    app_debug_adc_init();

    /* 12. 电流零点标定（须在无电流状态：桥臂零矢量且未旋转） */
    if (app_analog_signal_calibrate_offsets() == 0) {
        app_debug_printf("[ADC] zero calibration: OK\r\n");
    } else {
        app_debug_printf("[ADC] zero calibration: FAILED (default 1.65V in use)\r\n");
    }

    /* 13. FOC（电流环编排；上电 OFF，不使能输出） */
    app_foc_init();

    /* 14. 开环旋转自检（V/F，命令 r 启动） */
    app_debug_motor_init();
}

void app_run(void) {
    const app_hardware_params_t *hardware = app_hardware_params_current();

    /* 控制节拍 = 半桥开关频率（config/hardware.yaml；FOC 闭环同频） */
    const uint32_t cpu_freq = intf_clock_get_cpu_freq();
    const uint32_t loop_cycles = cpu_freq / hardware->inverter.pwm_freq_hz;
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

        /* 1) 核心：编码器双路采样（主循环节拍） */
        app_debug_encoder_sample();

        /* 1a) 模拟量：ADC 缓存 → 物理量换算 + 滤波（主循环节拍）+ Ozone 观测变量 */
        app_analog_signal_process();
        app_fault_process(); /* 主循环节拍：三相电流 RMS 累加（故障保护 L2） */

        /* 1a2) FOC 电流环（25kHz，与开关周期同频；OFF 时为空操作） */
        app_foc_run_once();
        app_debug_adc_update();

        /* 1b) 开环旋转（V/F）：FOC 活动时让位（互斥） */
        if (!app_foc_is_active()) {
            app_debug_motor_run_once();
        }

        now = intf_clock_get_cycle();

        /* 2) 低速调试任务（1ms 分频；各自内部有 1Hz 打印门限） */
        if ((uint32_t)(now - last_slow) >= slow_cycles) {
            last_slow = now;
            app_adc_slow_process(); /* ADC1 慢速通道（VBUS/NTC/CANID）@1kHz */
            app_debug_uart_run_once();
            app_debug_can_run_once();
            app_terminal_run_once(); /* USB Terminal（命令执行 + 输入 + job tick + TX flush） */
        }

        /* 3) 心跳 + 编码器统计汇总（1s） */
        if ((uint32_t)(now - last_hb) >= hb_cycles) {
            last_hb = now;
            did_heartbeat = true;
            heartbeat++;
            app_gpio_toggle(PIN_LED_STATUS);

#if APP_DEBUG_PERIODIC_PRINT
            {
                uint32_t c0 = intf_clock_get_cycle();
                app_debug_printf(
                    "hb=%u led=%u printf_cyc=%u\r\n", (unsigned)heartbeat,
                    (unsigned)app_gpio_read(PIN_LED_STATUS), (unsigned)last_printf_cycles);
                last_printf_cycles = intf_clock_get_cycle() - c0;
            }
            app_debug_encoder_print_stats();
#else
            (void)last_printf_cycles;
            (void)heartbeat;
#endif
        }

        /* 4) 主循环节拍：迟到计数并重同步（心跳轮次不计数） */
        now = intf_clock_get_cycle();
        {
            int32_t late = (int32_t)(now - next);

            if (late >= 0) {
                if (!did_heartbeat) {
                    app_debug_encoder_note_loop_late((uint32_t)late);
                }
                next = now + loop_cycles;
            }
        }
        while ((int32_t)(intf_clock_get_cycle() - next) < 0) {}
        next += loop_cycles;
    }
}
