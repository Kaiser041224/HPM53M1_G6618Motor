/**
 * @file    app_rtos.c
 * @brief   FreeRTOS 应用侧服务：致命钩子 + tick 基频自检
 * @author  Kaiser
 *
 * 设计文档：docs/superpowers/specs/2026-09-23-rtos-foundation-design.md
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_rtos.h"

#include <stdbool.h>
#include <stdint.h>

#include "app_3phase_inverter.h"
#include "app_debug_rtt.h"
#include "intf_clock.h"

#include "FreeRTOS.h"
#include "SEGGER_RTT.h"
#include "task.h"

/** tick 自检采样个数 */
#define APP_RTOS_TICK_SELFCHECK_SAMPLES (32U)
/** tick 自检允许偏差（0.1% 单位；20 = 2.0%） */
#define APP_RTOS_TICK_SELFCHECK_DEV_MAX (20U)

/**
 * @brief 致命错误处理：RTT 直写 → 紧急关断 → 停机。
 * @param file 触发文件名
 * @param line 触发行号
 */
void app_rtos_fatal(const char* file, long line) {
    /* 直写 RTT，不经日志队列（队列/生产者可能已损坏） */
    SEGGER_RTT_printf(0, "\r\n[FATAL] app_rtos: %s:%ld\r\n", (file != NULL) ? file : "?", line);

    app_3phase_inverter_emergency_stop();

    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

/**
 * @brief 栈溢出钩子（configCHECK_FOR_STACK_OVERFLOW = 2）。
 * @param task  任务句柄
 * @param name  任务名
 */
void vApplicationStackOverflowHook(TaskHandle_t task, char* name) {
    (void)task;
    SEGGER_RTT_WriteString(0, "\r\n[FATAL] stack overflow: ");
    SEGGER_RTT_WriteString(0, (name != NULL) ? name : "?");
    SEGGER_RTT_WriteString(0, "\r\n");
    app_rtos_fatal(__FILE__, __LINE__);
}

/**
 * @brief malloc 失败钩子（configUSE_MALLOC_FAILED_HOOK = 1）。
 */
void vApplicationMallocFailedHook(void) {
    SEGGER_RTT_WriteString(0, "\r\n[FATAL] pvPortMalloc failed\r\n");
    app_rtos_fatal(__FILE__, __LINE__);
}

/* ---------------------------------------------------------------------------
 * 异常指纹（诊断埋点，2026-09-23）
 *
 * 背景：FreeRTOS RISC-V 移植的 freertos_risc_v_application_exception_handler
 * 在 freertos_exception_handler() 之后无条件 `j .`（portASM.S:323），且 trap
 * 入口硬件已清 mstatus.MIE → 全系统静默冻结（RTT 无输出 / USB 不枚举 / ADC 死）。
 * SDK 弱实现（port.c:346）用 printf 打到 stdout，不经 SEGGER RTT，故 RTT 上
 * 零征兆。本处强符号覆盖之，只做取证，不改变行为（仍由调用方 `j .` 冻结）。
 *
 * 取证顺序（刻意）：
 *   1) 先用纯内联汇编读 CSR + 无函数调用写入 .noncacheable 全局
 *      —— 栈溢出场景下也尽量存活，冻结后可由 J-Link/Ozone 直接读；
 *   2) 再尝试 SEGGER RTT 直写（不经日志队列，队列可能已损坏）。
 * ------------------------------------------------------------------------- */

/** 异常指纹：mcause（bit31=中断，低 31 位=异常号） */
volatile uint32_t g_rtos_exc_mcause __attribute__((section(".noncacheable")));
/** 异常指纹：肇事指令地址 */
volatile uint32_t g_rtos_exc_mepc __attribute__((section(".noncacheable")));
/** 异常指纹：故障访问地址 / 非法指令内容 */
volatile uint32_t g_rtos_exc_mtval __attribute__((section(".noncacheable")));
/** 异常指纹：mstatus（含 MIE/MPIE/FS） */
volatile uint32_t g_rtos_exc_mstatus __attribute__((section(".noncacheable")));
/** 异常指纹：mscratch（FreeRTOS 嵌套 ISR 计数） */
volatile uint32_t g_rtos_exc_mscratch __attribute__((section(".noncacheable")));
/** 异常指纹：异常时刻 sp */
volatile uint32_t g_rtos_exc_sp __attribute__((section(".noncacheable")));
/** 异常指纹：异常时刻 ra */
volatile uint32_t g_rtos_exc_ra __attribute__((section(".noncacheable")));

/**
 * @brief 异常处理钩子（覆盖 port.c 弱实现；调用方随后 `j .` 冻结）。
 *
 * 仅取证：不调用 FreeRTOS API、不依赖调度器、不分配内存。
 */
void freertos_exception_handler(void) {
    uint32_t mcause;
    uint32_t mepc;
    uint32_t mtval;
    uint32_t mstatus;
    uint32_t mscratch;
    uint32_t sp;
    uint32_t ra;

    /* 1) 无函数调用取证（栈溢出场景也尽量安全） */
    __asm__ volatile("csrr %0, mcause" : "=r"(mcause));
    __asm__ volatile("csrr %0, mepc" : "=r"(mepc));
    __asm__ volatile("csrr %0, mtval" : "=r"(mtval));
    __asm__ volatile("csrr %0, mstatus" : "=r"(mstatus));
    __asm__ volatile("csrr %0, mscratch" : "=r"(mscratch));
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    __asm__ volatile("mv %0, ra" : "=r"(ra));

    g_rtos_exc_mcause = mcause;
    g_rtos_exc_mepc = mepc;
    g_rtos_exc_mtval = mtval;
    g_rtos_exc_mstatus = mstatus;
    g_rtos_exc_mscratch = mscratch;
    g_rtos_exc_sp = sp;
    g_rtos_exc_ra = ra;

    /* 2) 尝试 RTT 直写（不经队列）；失败也无妨，指纹已在 .noncacheable */
    SEGGER_RTT_printf(0, "\r\n[EXC] mcause=0x%08x mepc=0x%08x mtval=0x%08x\r\n", (unsigned)mcause,
                      (unsigned)mepc, (unsigned)mtval);
    SEGGER_RTT_printf(0,
                      "[EXC] mstatus=0x%08x mscratch=%u sp=0x%08x ra=0x%08x\r\n", (unsigned)mstatus,
                      (unsigned)mscratch, (unsigned)sp, (unsigned)ra);
    SEGGER_RTT_WriteString(0, "[EXC] frozen (freertos_risc_v_application_exception_handler)\r\n");
}

/**
 * @brief tick 基频自检（mcycle 实测 32 tick 平均周期 vs 1ms）。
 */
void app_rtos_selfcheck_tick(void) {
    const uint32_t samples = APP_RTOS_TICK_SELFCHECK_SAMPLES;
    const uint32_t expected_deci_us = 10000U; /* 1000.0 us */
    uint32_t cpu_freq;
    uint32_t start_cycle;
    uint32_t elapsed_cycles;
    uint32_t start_tick;
    uint32_t avg_deci_us;
    uint32_t dev_deci_us;
    uint32_t dev_pct_x10;
    bool pass;

    cpu_freq = intf_clock_get_cpu_freq();
    if (cpu_freq == 0U) {
        app_debug_printf("tick_selfcheck: FAIL, cpu_freq=0\r\n");
        return;
    }

    start_cycle = intf_clock_get_cycle();
    start_tick = xTaskGetTickCount();
    while ((uint32_t)(xTaskGetTickCount() - start_tick) < samples) {
        /* 等待 tick 推进 */
    }
    elapsed_cycles = intf_clock_get_cycle() - start_cycle;

    /* 平均周期，0.1us 单位：elapsed * 10^7 / (cpu_freq * samples) */
    avg_deci_us = (uint32_t)(((uint64_t)elapsed_cycles * 10000000ULL) /
                             ((uint64_t)cpu_freq * (uint64_t)samples));
    dev_deci_us = (avg_deci_us > expected_deci_us) ? (avg_deci_us - expected_deci_us) :
                                                    (expected_deci_us - avg_deci_us);
    dev_pct_x10 = (uint32_t)(((uint64_t)dev_deci_us * 1000ULL) / (uint64_t)expected_deci_us);
    pass = (dev_pct_x10 <= APP_RTOS_TICK_SELFCHECK_DEV_MAX);

    app_debug_printf(
        "tick_selfcheck: %s, avg=%u.%u us, dev=%u.%u%% (n=%u)\r\n", pass ? "PASS" : "FAIL",
        (unsigned)(avg_deci_us / 10U), (unsigned)(avg_deci_us % 10U),
        (unsigned)(dev_pct_x10 / 10U), (unsigned)(dev_pct_x10 % 10U), (unsigned)samples);
}
