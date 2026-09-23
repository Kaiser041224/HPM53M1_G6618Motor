/**
 * @file    app_logic.c
 * @brief   板级 bring-up：初始化编排 + FOC 快车道 / IO / 诊断步骤函数
 * @author  Kaiser
 *
 * 步骤函数（任务编排见 App/Logic/app_rtos_tasks.c）：
 *   - app_init()      ：一次性串行初始化（调度器后由 app_io 任务执行，此时 ISR 可用）
 *   - app_fast_step() ：FOC 硬实时快车道单步（25kHz）——采样 / 换算 / 保护 / 控制输出
 *                      硬约束：无 RTOS API、无 printf、无动态分配、无等待
 *   - app_io_step()   ：RTOS 后台 IO（1ms）——慢通道采样 + 调试观测 + 通讯轮询
 *   - app_diag_step() ：RTOS 后台诊断（1s）——回报（LED / 统计），不改控制状态
 *
 * 时钟：intf_clock_init() 已移至 main()，在调度器启动前完成
 *（对齐 SDK 惯例 board_init 含 board_init_clock；保证 MCHTMR tick 基频正确）。
 *
 * 说明：本文件为 bring-up 测试代码，后续由 FOC 应用替换。
 * 设计文档：docs/superpowers/specs/2026-09-23-foc-fastlane-task-structure-design.md
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
#include "app_gpio.h"
#include "app_hardware_params.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "app_terminal.h"
#include "app_usb.h"
#include "intf_clock.h"
#include "intf_sys.h"

/* 前向声明：hook 先于 step 定义（hook 注册在 app_init，step 定义在后） */
void app_fast_step(void);
static void app_fast_step_hook(void* user);

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

    /* 1. 系统时钟已在 main() 中、调度器启动前完成（intf_clock_init）：
     *    CPU 480MHz / AXI-AHB 160MHz / PLL0 960MHz / DCDC 1275mV */

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
        adc_cfg.fast_cb = app_fast_step_hook; /* 25kHz 控制环路（PMT 完成中断） */
        adc_cfg.fast_cb_user = NULL;
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

    /* 13. 开环旋转自检（V/F，命令 r 启动） */
    app_debug_motor_init();
}

/**
 * @brief FOC 快车道钩子（ADC PMT 完成中断内调用，25kHz 硬件触发）。
 *
 * 节拍由 PWM1 CMP10 → TRGM → ADC0 PMT 保证；ISR 不被 RTOS 任务/日志抢占。
 * 超时观测：执行时长超过一个节拍周期（40µs@25kHz）时记录 late。
 * @param user 用户上下文（未使用）
 */
static void app_fast_step_hook(void* user) {
    static uint32_t s_budget_cycles;
    uint32_t start_cycle;
    uint32_t elapsed;

    (void)user;

    if (s_budget_cycles == 0U) {
        const app_hardware_params_t* hardware = app_hardware_params_current();

        s_budget_cycles = intf_clock_get_cpu_freq() / hardware->inverter.pwm_freq_hz;
    }

    start_cycle = intf_clock_get_cycle();
    app_fast_step();
    elapsed = intf_clock_get_cycle() - start_cycle;

    /* 复用 late 指标：ISR 执行时间超过一个节拍周期（丢拍风险） */
    if (elapsed > s_budget_cycles) {
        app_debug_encoder_note_loop_late(elapsed - s_budget_cycles);
    }
}

void app_fast_step(void) {
    /* FOC 硬实时快车道单步（25kHz，ADC PMT 完成中断内执行）。
     * 硬约束：无 RTOS API、无 printf、无动态分配、无等待。
     * 内容 = FOC 强实时必需：采样 → 换算 → 保护 → 控制输出。
     * 将来 FOC 控制环替换 app_debug_motor_run_once 的位置。 */

    /* 1) 编码器双路采样（FOC 角度反馈） */
    app_debug_encoder_sample();

    /* 2) 模拟量：ADC 缓存 → 物理量换算 + 滤波（FOC 电流/电压反馈） */
    app_analog_signal_process();

    /* 3) 三相电流 RMS 累加 + L2 保护判断（FOC 保护） */
    app_fault_process();

    /* 4) 控制输出（开环 V/F；FOC 控制环原位替换点） */
    app_debug_motor_run_once();
}

void app_io_step(void) {
    /* RTOS 后台 IO 单步（1ms，由 app_io 任务调用）：
     * 慢通道采样 + 调试观测 + 通讯轮询。单次处理有界（≤200µs 约束不变）。 */

    /* ADC1 慢速通道采样（VBUS/NTC/CANID 原始码生产者） */
    app_adc_slow_process();

    /* Ozone 观测变量刷新（.noncacheable，1kHz 刷新，容忍撕裂） */
    app_debug_adc_update();

    /* 调试与通讯轮询（各自内部 1Hz 打印门限） */
    app_debug_uart_run_once();
    app_debug_can_run_once();
    app_terminal_run_once(); /* USB Terminal（命令执行 + 输入 + job tick + TX flush） */
}

void app_diag_step(void) {
    /* RTOS 后台诊断单步（1s，由 app_diag 任务调用）：
     * 纯回报，不改控制状态。 */

    app_gpio_toggle(PIN_LED_STATUS);
    app_debug_encoder_print_stats();
}
