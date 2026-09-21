/**
 * @file    irq_profiler.h
 * @brief   IRQ Profiler 中断耗时分析（低侵入 ISR timing）
 * @author  Kaiser
 *
 * Designed for 200kHz/400kHz PWM/ADC fast-loop control on HPM RISC-V MCU.
 * ISR overhead: ~10 cycles (2 reads + 2 writes + 1 branch).
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef IRQ_PROFILER_H
#define IRQ_PROFILER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration - user can override via build system
 * ============================================================================ */

/* Set to 0 to completely disable profiler (macros become empty) */
#ifndef IRQ_PROF_ENABLED
#define IRQ_PROF_ENABLED 1
#endif

/* Set to 1 to enable GPIO pulse output for oscilloscope verification */
#ifndef IRQ_PROF_GPIO_ENABLED
#define IRQ_PROF_GPIO_ENABLED 0
#endif

/* Outlier threshold in cycles (480MHz * 10us = 4800 cycles) */
#ifndef IRQ_PROF_OUTLIER_CYCLES
#define IRQ_PROF_OUTLIER_CYCLES 4800
#endif

/* Max measurement slots */
#ifndef IRQ_PROF_MAX_SLOTS
#define IRQ_PROF_MAX_SLOTS 16
#endif

/* ============================================================================
 * Types - keep minimal for fast ISR writes
 * ============================================================================ */

/**
 * @brief 测量槽位 ID（0..IRQ_PROF_MAX_SLOTS-1；UINT8_MAX = 注册失败）
 */
typedef uint8_t  irq_prof_id_t;

/**
 * @brief 周期计数类型（CPU cycle）
 */
typedef uint32_t irq_prof_cycle_t;

/* Raw stat stored in ISR context - no float, no conversion */

/**
 * @brief ISR 上下文原始统计（无浮点、无换算）
 */
typedef struct {
    irq_prof_cycle_t last;     /**< 最近一次耗时 [cycle] */
    irq_prof_cycle_t min;      /**< 最小耗时 [cycle] */
    irq_prof_cycle_t max;      /**< 最大耗时 [cycle] */
    uint64_t         total;    /**< 有效样本累计 [cycle] */
    uint32_t         hits;     /**< 命中次数 */
    uint32_t         outliers; /**< 离群次数（> IRQ_PROF_OUTLIER_CYCLES） */
} irq_prof_raw_t;

/* Converted result for dump context only */

/**
 * @brief 主上下文换算结果（单位 ns）
 */
typedef struct {
    uint32_t last_ns;     /**< 最近耗时 [ns] */
    uint32_t min_ns;      /**< 最小耗时 [ns] */
    uint32_t max_ns;      /**< 最大耗时 [ns] */
    uint32_t avg_ns;      /**< 平均耗时 [ns] */
    uint32_t hits;        /**< 命中次数 */
    uint32_t outliers;    /**< 离群次数 */
    uint32_t overhead_ns; /**< 测量开销 [ns] */
} irq_prof_result_t;

/* ============================================================================
 * Hardware abstraction - weak hooks for GPIO observation
 * ============================================================================ */

/**
 * @brief GPIO 置高钩子（示波器观测，板级实现，弱符号）
 * @param slot_id 槽位 ID
 */
void irq_prof_gpio_set(uint8_t slot_id)   __attribute__((weak));

/**
 * @brief GPIO 拉低钩子（示波器观测，板级实现，弱符号）
 * @param slot_id 槽位 ID
 */
void irq_prof_gpio_clear(uint8_t slot_id) __attribute__((weak));

/* ============================================================================
 * Core API - must be inline for minimal ISR overhead
 * ============================================================================ */

/**
 * @brief 读取当前 CPU cycle 计数（mcycle CSR）
 * @return 当前 mcycle 值
 */
static inline irq_prof_cycle_t irq_prof_read_cycle(void)
{
    irq_prof_cycle_t val;
    __asm__ volatile("csrr %0, mcycle" : "=r"(val));
    return val;
}

/**
 * @brief 注册测量槽位
 * @param label 槽位标签（NULL 存为 "???"）
 * @return 槽位 ID；UINT8_MAX = 槽位耗尽
 */
irq_prof_id_t irq_prof_register(const char *label);

/**
 * @brief ISR 进入：记录起始 cycle
 * @param id 槽位 ID
 */
static inline void irq_prof_enter(irq_prof_id_t id)
{
    extern volatile irq_prof_cycle_t g_irq_prof_stamp[IRQ_PROF_MAX_SLOTS];
    if (id < IRQ_PROF_MAX_SLOTS) {
        g_irq_prof_stamp[id] = irq_prof_read_cycle();
    }
}

/**
 * @brief ISR 退出：累加耗时统计（离群样本单列）
 * @param id 槽位 ID
 */
static inline void irq_prof_exit(irq_prof_id_t id)
{
    extern volatile irq_prof_raw_t   g_irq_prof_raw[IRQ_PROF_MAX_SLOTS];
    extern volatile irq_prof_cycle_t g_irq_prof_stamp[IRQ_PROF_MAX_SLOTS];

    if (id >= IRQ_PROF_MAX_SLOTS) {
        return;
    }

    irq_prof_cycle_t elapsed = irq_prof_read_cycle() - g_irq_prof_stamp[id];

    volatile irq_prof_raw_t *r = &g_irq_prof_raw[id];
    r->last = elapsed;
    r->hits++;

    if (elapsed > IRQ_PROF_OUTLIER_CYCLES) {
        r->outliers++;
        return;
    }

    r->total += elapsed;
    if (elapsed < r->min) {
        r->min = elapsed;
    }
    if (elapsed > r->max) {
        r->max = elapsed;
    }
}

/**
 * @brief GPIO 置高（示波器观测；未启用时为空操作）
 * @param id 槽位 ID
 */
static inline void irq_prof_gpio_high(irq_prof_id_t id)
{
#if IRQ_PROF_GPIO_ENABLED
    extern void irq_prof_gpio_set(uint8_t slot_id);
    if (irq_prof_gpio_set) {
        irq_prof_gpio_set(id);
    }
#else
    (void)id;
#endif
}

/**
 * @brief GPIO 拉低（示波器观测；未启用时为空操作）
 * @param id 槽位 ID
 */
static inline void irq_prof_gpio_low(irq_prof_id_t id)
{
#if IRQ_PROF_GPIO_ENABLED
    extern void irq_prof_gpio_clear(uint8_t slot_id);
    if (irq_prof_gpio_clear) {
        irq_prof_gpio_clear(id);
    }
#else
    (void)id;
#endif
}

/**
 * @brief 读取槽位测量结果快照并换算为 ns
 * @param id 槽位 ID
 * @param result 输出结果（不可为 NULL）
 * @return 0 = 成功；-1 = 槽位越界或 result 为 NULL
 */
int  irq_prof_get_result(irq_prof_id_t id, irq_prof_result_t *result);

/**
 * @brief 获取已注册槽位数量
 * @return 槽位数量
 */
uint8_t irq_prof_get_slot_count(void);

/**
 * @brief 获取槽位标签
 * @param id 槽位 ID
 * @return 标签字符串（未注册返回 "???"）
 */
const char *irq_prof_get_label(irq_prof_id_t id);

/**
 * @brief 获取测量开销（cycle）
 * @return 测量开销 [cycle]
 */
irq_prof_cycle_t irq_prof_get_overhead_cycles(void);

/**
 * @brief 标定测量开销（取 100 次连续读取的最小差值）
 * @return 测量开销 [cycle]
 */
irq_prof_cycle_t irq_prof_measure_overhead(void);

/* [TEMP DIAG] 嵌套感知的中断总占用测量。每个 ISR 最外层入口调 enter、出口调 exit，
 * 内层嵌套自动不重复计。g_irq_busy_cycles = CPU 处于中断态的真实墙钟 cycle。 */
extern volatile uint64_t g_irq_busy_cycles;

/**
 * @brief 嵌套感知中断占用：最外层 ISR 进入
 */
void irq_prof_nest_enter(void);

/**
 * @brief 嵌套感知中断占用：最外层 ISR 退出
 */
void irq_prof_nest_exit(void);

/* ============================================================================
 * Convenience macros - empty when disabled
 * ============================================================================ */

#if IRQ_PROF_ENABLED
    #define IRQ_PROF_ENTER(id)           irq_prof_enter(id)
    #define IRQ_PROF_EXIT(id)            irq_prof_exit(id)
    #define IRQ_PROF_GPIO_HIGH(id)       irq_prof_gpio_high(id)
    #define IRQ_PROF_GPIO_LOW(id)        irq_prof_gpio_low(id)
#else
    #define IRQ_PROF_ENTER(id)           do { (void)(id); } while (0)
    #define IRQ_PROF_EXIT(id)            do { (void)(id); } while (0)
    #define IRQ_PROF_GPIO_HIGH(id)       do { (void)(id); } while (0)
    #define IRQ_PROF_GPIO_LOW(id)        do { (void)(id); } while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* IRQ_PROFILER_H */
