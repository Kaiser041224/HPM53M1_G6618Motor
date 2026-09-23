/**
 * @file    app_logic.c
 * @brief   初始化编排 + FOC 快车道钩子 / IO / 诊断步骤函数
 * @author  Kaiser
 *
 * 步骤函数（任务编排见 App/Logic/app_rtos_tasks.c）：
 *   - app_init()           ：一次性串行初始化（调度器后由 app_io 任务执行，此时 ISR 可用）
 *   - app_fast_step_hook() ：FOC 硬实时快车道（25kHz，ADC PMT 完成中断内）
 *                            mcycle 计时包装 → app_foc_isr_step
 *                            硬约束：无 RTOS API、无 printf、无动态分配、无等待
 *   - app_io_step()        ：RTOS 后台 IO（1ms）——模拟量/故障/状态机编排 + 通讯轮询
 *   - app_diag_step()      ：RTOS 后台诊断（1s）——回报（LED / 统计），不改控制状态
 *
 * 时钟：intf_clock_init() 在 main()、调度器启动前完成（对齐 SDK 惯例，
 *      保证 MCHTMR tick 基频正确）。设计文档：
 *      docs/superpowers/specs/2026-09-23-foc-v1-rtos-merge-design.md
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
#include "app_debug_foc.h"
#include "app_debug_inverter.h"
#include "app_debug_rtt.h"
#include "app_debug_uart.h"
#include "app_encoder.h"
#include "app_fault.h"
#include "app_foc.h"
#include "app_gpio.h"
#include "app_gptmr.h"
#include "app_hardware_params.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "app_terminal.h"
#include "app_usb.h"
#include "intf_adc.h"
#include "intf_clock.h"
#include "intf_sys.h"

/* 前向声明：hook 注册于 app_init 末尾（覆盖 app_foc_init 的自注册），定义在后 */
static void app_fast_step_hook(void);

/* ISR 耗时统计打印（app_diag_step 调用；定义在文件尾部） */
void app_isr_stats_print(void);

/* ============================================================================
 * ISR 耗时统计
 *   step 段 = app_foc_isr_step（FOC 控制段，hook 内测量）
 *   isr 段  = adc_generic_isr 整体（drv_adc 内测量：PMT 校验 + 控制段回调）
 * ISR 内只做廉价 min/max/累加；app_diag_step 每秒打印并清零窗口。
 * ========================================================================== */
static uint32_t s_step_min_cy = 0xFFFFFFFFU;
static uint32_t s_step_max_cy;
static uint32_t s_step_sum_cy;
static uint32_t s_step_cnt;

void app_init(void) {
    /*
     * 0. 复位诊断：
     *    - s_boot_seq 位于 NOLOAD 段（复位不清零）：逐次递增 = 芯片在复位循环。
     *    - rst_status 为 PPOR.RESET_STATUS（只读）：bit0=欠压、bit4=调试复位、
     *      bit16/17=看门狗、bit24=PMIC 看门狗、bit31=软件复位。
     */
    static volatile uint32_t s_boot_seq __attribute__((section(".noncacheable")));
    uint32_t rst_status;

    s_boot_seq++;
    rst_status = intf_sys_get_reset_status();
    app_debug_printf(
        "boot: seq=%u rst_status=0x%08x\r\n", (unsigned)s_boot_seq, (unsigned)rst_status);

    /* 0b. 参数单例初始化 + 摘要（验证 YAML 参数管线端到端） */
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

    /* 3. 频率自检（寄存器读数） */
    app_debug_printf(
        "clock: cpu=%u Hz, ahb=%u Hz\r\n", (unsigned)intf_clock_get_cpu_freq(),
        (unsigned)intf_clock_get_ahb_freq());

    /* 4. UART0 自检（PA00/PA01，115200 8N1） */
    app_debug_uart_init();

    /* 5. CAN 自检（MCAN3，经典 CAN） */
    app_debug_can_init();

    /* 6. USB 终端（USB0 CDC 虚拟串口；CherrySH Terminal，1kHz 慢任务轮询） */
    app_usb_init();
    app_terminal_init();

    /* 7. 编码器自检（双 KTH7823：SPI3 转子 / SPI1 出轴） */
    app_debug_encoder_init();

    /* 7b. GPTMR 外设 + 12.5kHz 转子采样器（GPTMR1 CH3，PLIC prio 3 > ADC0）。
     *     此后 SPI3 归采样 ISR 独占，运行期读 API 只返回一致快照。 */
    app_gptmr_init();
    {
        int sampler_rc = app_encoder_sampler_start();

        app_debug_printf("[ENC] sampler: %s (GPTMR1 CH3 @12.5kHz)\r\n",
                         (sampler_rc == 0) ? "OK" : "FAILED");
    }

    /* 8. Flash 自检（XPI NOR：属性 + 末尾扇区破坏性读写测试） */
    app_debug_flash_init();

    /* 9. 三相半桥输出自检（PWM1：默认频率 / 50% 持续输出） */
    app_debug_inverter_init();

    /* 10. 故障保护（在 ADC 之前：提供 WDOG 阈值与回调） */
    app_fault_init(NULL);

    /* 11. ADC 采样链（PWM1 CMP10 触发 → TRGM → 双 ADC PMT：三相电流 + 母线/NTC；
     *     ADC0 电流通道启用 WDOG；ADC1 序列完成回调接入故障 tick @1kHz） */
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

    /* 13b. FOC 调试观测/命令（Ozone 结构；io 任务有界处理请求） */
    app_debug_foc_init();

    /* 13c. 快车道钩子：覆盖 app_foc_init 内的直注册，注入 mcycle 计时包装 */
    app_adc_register_current_hook(app_fast_step_hook);
}

/**
 * @brief FOC 快车道钩子（ADC0 PMT 完成中断内调用，25kHz 硬件触发）。
 *
 * 节拍由 PWM1 CMP10 → TRGM → ADC0 PMT 保证；ISR 不被 RTOS 任务/日志抢占。
 * 仅做 mcycle 计时包装 + 耗时统计；控制语义全在 app_foc_isr_step（foc-v1 原样）。
 * 超预算（> 1 个节拍周期）记 late，供 [ENC]/[ISR] 统计。
 */
static void app_fast_step_hook(void) {
    static uint32_t s_budget_cycles;
    uint32_t start_cycle;
    uint32_t elapsed;

    if (s_budget_cycles == 0U) {
        s_budget_cycles = intf_clock_get_cpu_freq()
                        / app_hardware_params_current()->inverter.pwm_freq_hz;
    }

    start_cycle = intf_clock_get_cycle();
    app_foc_isr_step();
    elapsed = intf_clock_get_cycle() - start_cycle;

    /* step 段耗时统计（1s 窗口；app_diag_step 读取并清零） */
    s_step_cnt++;
    s_step_sum_cy += elapsed;
    if (elapsed > s_step_max_cy) {
        s_step_max_cy = elapsed;
    }
    if (elapsed < s_step_min_cy) {
        s_step_min_cy = elapsed;
    }

    /* ISR 执行时间超过一个节拍周期（丢拍风险） */
    if (elapsed > s_budget_cycles) {
        app_debug_encoder_note_loop_late(elapsed - s_budget_cycles);
    }
}

void app_io_step(void) {
    /* RTOS 后台 IO 单步（1ms，由 app_io 任务调用）：单次处理有界（≤200µs 约束不变）。
     * 顺序：观测刷新 → 模拟量换算 → 故障 RMS → 状态机编排 → 通讯轮询。 */

    /* 转子观测变量刷新（缓存读，零 SPI 成本；出轴采样已裁，spec §3） */
    app_debug_encoder_sample();

    /* 模拟量：ADC 缓存 → 物理量换算 + 滤波（FOC 的 v_bus 亦取自本模块缓存） */
    app_analog_signal_process();

    /* 故障保护 L2/L3：RMS 累加 + 去抖判断（M1 动作全关、仅检测/告警，spec §6） */
    app_fault_process();

    /* FOC 状态机 / 给定编排（控制环在 ISR，见 app_foc_isr_step） */
    app_foc_run_once();

    /* Ozone 命令消费 + 状态域刷新（有界：≤2 条命令/拍） */
    app_debug_foc_tick();

    /* ADC1 慢速通道（VBUS/NTC/CANID 原始码生产者） */
    app_adc_slow_process();

    /* Ozone 观测变量刷新（.noncacheable，1ms 刷新，容忍撕裂） */
    app_debug_adc_update();

    /* 调试与通讯轮询（各自内部 1Hz 打印门限） */
    app_debug_uart_run_once();
    app_debug_can_run_once();
    app_terminal_run_once(); /* USB Terminal（命令执行 + 输入 + job tick + TX flush） */
}

void app_diag_step(void) {
    /* RTOS 后台诊断单步（1s，由 app_diag 任务调用）：纯回报，不改控制状态。 */
    app_gpio_toggle(PIN_LED_STATUS);
    app_debug_encoder_print_stats();
    app_isr_stats_print();
}

/**
 * @brief ISR 耗时统计打印（1s 窗口，由 app_diag_step 调用）
 *
 * 输出（单位 µs，一位小数）：
 *   step min/avg/max = app_foc_isr_step（FOC 控制段）
 *   isr  avg/peak    = adc_generic_isr 整体（PMT 校验 + 控制段；peak 每窗复位）
 *   headroom         = 40µs 节拍预算 − isr peak
 *   load             = isr 段 CPU 占用率（窗口内 delta，0.1% 单位）
 * 注：isr 段不含 FreeRTOS 上下文保存/恢复与 PLIC claim/complete（另约 1–2µs）。
 */
void app_isr_stats_print(void) {
    static uint64_t s_prev_isr_total;
    static uint32_t s_prev_isr_cnt;
    static uint32_t s_prev_cycle;
    static bool s_inited;

    intf_adc_diag_snapshot_t snap;
    uint32_t cycle_now;
    uint32_t window_cy;
    uint32_t div;
    uint32_t step_min;
    uint32_t step_avg;
    uint32_t step_max;
    uint32_t isr_avg;
    uint32_t isr_peak;
    uint32_t isr_cnt;
    uint32_t load_x10;
    uint32_t budget_cy;
    uint32_t headroom_cy;
    uint32_t status;

    /* 临界区快照 + 清零 step 统计（防止 ISR 并发写导致读数撕裂） */
    status = intf_sys_irq_save();
    step_min = s_step_min_cy;
    step_max = s_step_max_cy;
    step_avg = (s_step_cnt != 0U) ? (s_step_sum_cy / s_step_cnt) : 0U;
    s_step_min_cy = 0xFFFFFFFFU;
    s_step_max_cy = 0U;
    s_step_sum_cy = 0U;
    isr_cnt = s_step_cnt;
    s_step_cnt = 0U;
    intf_sys_irq_restore(status);

    if (intf_adc_get_diag_snapshot(&snap) != 0) {
        app_debug_printf("[ISR] diag unavailable\r\n");
        return;
    }

    cycle_now = intf_clock_get_cycle();
    window_cy = s_inited ? (uint32_t)(cycle_now - s_prev_cycle) : 0U;
    div = intf_clock_get_cpu_freq() / 1000000U; /* cycles per µs */
    if (div == 0U) {
        div = 1U;
    }
    budget_cy = (intf_clock_get_cpu_freq()
                 / app_hardware_params_current()->inverter.pwm_freq_hz);

    /* isr 段窗口均值（ADC0 = FOC 快车道中断）与本窗峰值 */
    isr_avg = 0U;
    load_x10 = 0U;
    if (s_inited && (isr_cnt > 0U)) {
        uint64_t delta_total = snap.isr_total_cycles[0] - s_prev_isr_total;
        uint32_t delta_cnt = snap.irq_entry[0] - s_prev_isr_cnt;

        if (delta_cnt > 0U) {
            isr_avg = (uint32_t)(delta_total / delta_cnt);
        }
        if (window_cy > 0U) {
            /* 0.1% 单位：占用率 × 1000（此前误用 ×10，读数偏小 100 倍） */
            load_x10 = (uint32_t)((delta_total * 1000ULL) / window_cy);
        }
    }
    isr_peak = snap.isr_cycles_max[0];
    headroom_cy = (budget_cy > isr_peak) ? (budget_cy - isr_peak) : 0U;

    app_debug_printf(
        "[ISR] n=%u | step min/avg/max=%u.%u/%u.%u/%u.%u | isr avg=%u.%u peak=%u.%u | "
        "budget=%u.%u headroom=%u.%u | load=%u.%u%%\r\n",
        (unsigned)isr_cnt, (unsigned)(step_min / div), (unsigned)((step_min % div) * 10U / div),
        (unsigned)(step_avg / div), (unsigned)((step_avg % div) * 10U / div),
        (unsigned)(step_max / div), (unsigned)((step_max % div) * 10U / div),
        (unsigned)(isr_avg / div), (unsigned)((isr_avg % div) * 10U / div),
        (unsigned)(isr_peak / div), (unsigned)((isr_peak % div) * 10U / div),
        (unsigned)(budget_cy / div), (unsigned)((budget_cy % div) * 10U / div),
        (unsigned)(headroom_cy / div), (unsigned)((headroom_cy % div) * 10U / div),
        (unsigned)(load_x10 / 10U), (unsigned)(load_x10 % 10U));

    /* 峰值按窗复位：否则开机瞬态（如标定期）污染 headroom 至关机 */
    intf_adc_reset_diag_max();

    s_prev_isr_total = snap.isr_total_cycles[0];
    s_prev_isr_cnt = snap.irq_entry[0];
    s_prev_cycle = cycle_now;
    s_inited = true;
}
