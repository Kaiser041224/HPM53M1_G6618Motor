/**
 * @file    app_motion_timer.c
 * @brief   Motion 单 15kHz 实时拍编排，编码器只读 ADC 快照
 * @author  Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_motion_timer.h"
#include "app_encoder.h"
#include "app_gptmr.h"
#include "app_ramfunc.h"
#include "intf_clock.h"
#include "intf_gptmr.h"
#include "intf_sys.h"
#include <stddef.h>
/* ── 实时拍时间片（2026-10-30 定稿）────────────────────────────────────
 * 两个周期中断，一条时钟链（drv_clock：PWM = PLL0CLK0/6 = 160MHz，
 * GPTMR1 = PLL0CLK0/12 = 80MHz），速率严格 2:1 锁定，相位恒定：
 *
 *   FOC 帧 k                  FOC 帧 k+1                FOC 帧 k+2
 *   │                         │                         │
 *   0──────────17──18────24.5─33.3──────────────────────66.66 µs
 *   ├─ ADC0 PMT ISR ─┤        ├─ ADC0 PMT ISR ─┤
 *   │ 取帧→observer→电流环→齿槽（实测 avg 16.6 / peak 17.0µs，预算 25µs）
 *                    ├ motion ┤
 *                    （相位 18µs 起，实测耗时 ≤6.5µs，尾部余 8.8µs）
 *
 * 片内分工：
 *   ADC0 PMT（prio 3，30kHz）：双编码器同步取帧 → observer → 电流环 → 齿槽注入
 *   GPTMR1 CH0（prio 2，15kHz）：位置环 → 速度环 → 提交下一拍 iq
 *   其余（USB/CAN/UART/GPIO，prio 1）：事件驱动，必须短；ADC1 无中断，PWM 中断仅调试用
 *
 * 相位必须由软件安排（motion_timer_start_aligned）：DISABLE_IRQ_PREEMPTIVE=1 下两者
 * 不能互相抢占，相位重叠即互相推迟。
 *   ≈0µs  → 每拍与 FOC 入口撞车：编码器配对龄越限 → SAMPLE 拒绝 → 零力矩
 *   ≈18µs → FOC（实测 peak 17.0µs）刚结束，observer 速度已发布；配对龄 18µs 远在
 *            APP_ENCODER_OUTPUT_MAX_AGE_US（74µs）内；motion 尾部离下一帧 8.8µs
 *   ≥26µs → motion 尾部顶到下一个 FOC 帧入口，两个 ISR 交替推迟对方
 *            （2026-10-30 实测：相位 26µs 时 period 在 38..95µs 之间长短交替，
 *             [MOTION] late 每秒 1~3 次）。下界由 FOC 耗时定，上界由 motion 耗时定。
 * 为何是编译期常量而非 YAML：SPRM 记录已用 236/240 字节（结构体 232 + 记录头 4，
 * 见 app_software_params.c 的 _Static_assert），只剩一个 4 字节字段的余量，
 * 留给控制整定量更合理；
 * 本值属时序设计常量，改动须连带复核 [MOTION] age/slice 实测。 */
#define APP_MOTION_TIMER_PHASE_US         (18U)
#define APP_MOTION_TIMER_ALIGN_TIMEOUT_US (2000U)
/** @brief 对齐忙等迭代上限：mcycle 正常前进时用不到（时间上限由 cycle 判据给出），
 *         仅防御时钟读数不前进（异常 / 主机 mock 时钟）把 app_init 卡死 */
#define APP_MOTION_TIMER_ALIGN_SPIN_MAX   (100000U)
/** @brief FOC 帧标称频率 [Hz]（帧内余量换算用；实际 30001.875Hz，差异不影响判定） */
#define APP_MOTION_TIMER_FOC_RATE_HZ      (30000U)

static app_motion_timer_snapshot_t s_motion_timer_snapshot;
static uint32_t s_previous_cycle;
static uint32_t s_expected_cycles;
static uint32_t s_frame_cycles;     /**< 一个 FOC 帧的 CPU cycle 数（帧内余量判定基准） */
static uint32_t s_entry_age_cycles; /**< 本拍入口处的反馈龄 = 帧内相位（slice 判定用） */
static bool s_motion_timer_initialized;
static void (*s_tick_callback)(void);
void app_motion_timer_bind_tick(void (*callback)(void)) {
    uint32_t st = intf_sys_irq_save();
    s_tick_callback = callback;
    intf_sys_irq_restore(st);
}
/** @brief 只观测最新编码器快照和入口间隔 */
APP_ATTR_RAMFUNC
static void motion_timer_observe(void) {
    app_motion_timer_channel_snapshot_t* snapshot = &s_motion_timer_snapshot.tick;
    uint32_t start = intf_clock_get_cycle();
    if (snapshot->count != 0) {
        uint32_t period = start - s_previous_cycle;
        if (snapshot->period_min_cycles == 0 || period < snapshot->period_min_cycles)
            snapshot->period_min_cycles = period;
        if (period > snapshot->period_max_cycles)
            snapshot->period_max_cycles = period;
        if (period > s_expected_cycles + s_expected_cycles / 10U) {
            snapshot->late_count++;
            snapshot->window_late_count++;
        }
    }
    s_previous_cycle = start;
    snapshot->count++;
    snapshot->window_count++;
    app_encoder_rotor_snapshot_t encoder;
    int rc = app_encoder_get_rotor_snapshot(&encoder);
    snapshot->feedback_valid = rc == 0 && encoder.valid;
    s_entry_age_cycles = 0U;
    if (rc == 0) {
        s_entry_age_cycles = encoder.age_cycles;
        /* 入口处的反馈龄 = 本拍落在 FOC 帧内的相位：锁相成立时 min≈max。
         * 窗内 min/max 是时间片是否守住的直接证据（滑移会把两者拉到 0..一帧）。 */
        snapshot->feedback_sequence = encoder.seq;
        snapshot->feedback_age_cycles = encoder.age_cycles;
        if (snapshot->feedback_age_min_cycles == 0
            || encoder.age_cycles < snapshot->feedback_age_min_cycles)
            snapshot->feedback_age_min_cycles = encoder.age_cycles;
        if (encoder.age_cycles > snapshot->feedback_age_max_cycles)
            snapshot->feedback_age_max_cycles = encoder.age_cycles;
    }
    uint32_t elapsed = intf_clock_get_cycle() - start;
    if (elapsed > snapshot->execution_max_cycles)
        snapshot->execution_max_cycles = elapsed;
}
/** @brief 单 15kHz 拍：app_motion_tick 内部完成 position → velocity 编排 */
APP_ATTR_RAMFUNC
static void motion_timer_tick(void) {
    uint32_t start = intf_clock_get_cycle();
    motion_timer_observe();
    if (s_tick_callback)
        s_tick_callback();
    uint32_t elapsed = intf_clock_get_cycle() - start;
    app_motion_timer_channel_snapshot_t* snapshot = &s_motion_timer_snapshot.tick;
    if (elapsed > snapshot->execution_max_cycles)
        snapshot->execution_max_cycles = elapsed;
    /* 判据用实测相位（入口反馈龄）而非设定值：入口龄 + 本拍耗时 > 一个 FOC 帧
     * ⇒ 本拍尾部压到下一个 FOC 帧入口上，非抢占下两个 ISR 会交替推迟对方
     * （表现为 period 长短交替 + late）。出现即须重排时间片：调 PHASE_US，
     * 或把 motion 的某段移出热路径。 */
    if ((s_frame_cycles != 0U) && (s_entry_age_cycles != 0U)
        && ((s_entry_age_cycles + elapsed) > s_frame_cycles)) {
        snapshot->slice_overrun_count++;
        snapshot->window_slice_overrun_count++;
    }
}
int app_motion_timer_init(void) {
    app_motion_timer_stop();
    s_motion_timer_initialized = false;
    s_motion_timer_snapshot = (app_motion_timer_snapshot_t){0};
    s_expected_cycles = intf_clock_get_cpu_freq() / 15000U;
    s_frame_cycles = intf_clock_get_cpu_freq() / APP_MOTION_TIMER_FOC_RATE_HZ;
    s_entry_age_cycles = 0U;
    intf_gptmr_cfg_t motion = {
        .mode = INTF_GPTMR_MODE_TIMER, .frequency_hz = 15000, .callback = motion_timer_tick};
    if (intf_gptmr_init(4, &motion) != 0) {
        app_motion_timer_stop();
        return -1;
    }
    app_motion_timer_stop();
    s_motion_timer_initialized = true;
    return 0;
}
/**
 * @brief 对齐到 FOC 帧内相位后放行计数器（与 PWM 锁相）
 * @return app_gptmr_start 的返回值
 * @note 任务上下文调用（app_init 在 io 任务内），忙等 ≤ 一帧 + 超时窗。
 *       转子快照 timestamp_cycles 即 FOC ISR 入口 mcycle（app_encoder_sync_sample(t_isr)）。
 *       观测不到新帧（FOC ISR 未跑/超时）时退化为直接启动，相位随机但速率仍锁定。
 */
static int motion_timer_start_aligned(void) {
    uint32_t cycles_per_us = intf_clock_get_cpu_freq() / 1000000U;
    uint32_t timeout_cycles = cycles_per_us * APP_MOTION_TIMER_ALIGN_TIMEOUT_US;
    uint32_t phase_cycles = cycles_per_us * APP_MOTION_TIMER_PHASE_US;
    uint32_t frame_cycles = intf_clock_get_cpu_freq() / APP_MOTION_TIMER_FOC_RATE_HZ;
    uint32_t spin = APP_MOTION_TIMER_ALIGN_SPIN_MAX;
    uint32_t begin;
    app_encoder_rotor_snapshot_t encoder;
    uint32_t seq;

    /* app_gptmr_start 走 XIP（app → intf → drv 三层 + 寄存器写）：首次执行 I-cache 冷，
     * 2026-10-30 实测把落点推后约 9µs（设定 17µs → 实测 age 均值 25.8µs），直接
     * 毁掉相位安排。先空跑两次：第一次预热 I-cache，第二次实测热态耗时，
     * 再从目标时刻里扣掉。计数器每次都立即停掉，66.66µs 内不可能产生回调。 */
    (void)app_gptmr_start(APP_GPTMR_CH_0); /* 第一次：预热 I-cache，耗时不参与计算 */
    (void)app_gptmr_stop(APP_GPTMR_CH_0);
    /* 实测段必须关中断：若被 FOC ISR（17µs）插入，量到的是中断耗时，
     * 补偿反而把相位打到 0（= 每拍撞车）。热态本段 ≈1µs，对 FOC 无影响。 */
    uint32_t irq_state = intf_sys_irq_save();
    uint32_t probe = intf_clock_get_cycle();
    (void)app_gptmr_start(APP_GPTMR_CH_0);
    uint32_t start_cycles = intf_clock_get_cycle() - probe;
    intf_sys_irq_restore(irq_state);
    (void)app_gptmr_stop(APP_GPTMR_CH_0);
    phase_cycles = (phase_cycles > start_cycles) ? (phase_cycles - start_cycles) : 0U;

    begin = intf_clock_get_cycle();
    if (app_encoder_get_rotor_snapshot(&encoder) != 0)
        return app_gptmr_start(APP_GPTMR_CH_0);
    seq = encoder.seq;
    /* 轮询到序号翻新的那一拍：此刻 timestamp_cycles 是刚发生的 FOC 帧入口 */
    while (!((app_encoder_get_rotor_snapshot(&encoder) == 0) && (encoder.seq != seq))) {
        if (((intf_clock_get_cycle() - begin) > timeout_cycles) || (--spin == 0U))
            return app_gptmr_start(APP_GPTMR_CH_0);
    }
    /* 等相位与放行同在一个关中断窗口内：否则 RTOS tick / 其他 ISR 插在中间，
     * 落点相位就带上那段不可控延迟。窗口 ≤ PHASE_US + start 路径（约 20µs）。
     * 目标时刻 = 帧入口 + PHASE_US；发现得太晚则顺延整数个 FOC 帧。 */
    uint32_t state = intf_sys_irq_save();
    uint32_t target = encoder.timestamp_cycles + phase_cycles;
    spin = APP_MOTION_TIMER_ALIGN_SPIN_MAX;
    while (((int32_t)(intf_clock_get_cycle() - target) > 0) && (--spin != 0U))
        target += frame_cycles;
    spin = APP_MOTION_TIMER_ALIGN_SPIN_MAX;
    while (((int32_t)(intf_clock_get_cycle() - target) < 0) && (--spin != 0U)) {
    }
    int rc = app_gptmr_start(APP_GPTMR_CH_0);
    intf_sys_irq_restore(state);
    return rc;
}
int app_motion_timer_start(void) {
    if (!s_motion_timer_initialized)
        return -1;
    app_motion_timer_stop();
    s_motion_timer_snapshot = (app_motion_timer_snapshot_t){0};
    if (motion_timer_start_aligned() != 0) {
        app_motion_timer_stop();
        return -1;
    }
    return 0;
}
void app_motion_timer_stop(void) {
    (void)app_gptmr_stop(APP_GPTMR_CH_0);
    (void)app_gptmr_stop(APP_GPTMR_CH_1);
    (void)app_gptmr_stop(APP_GPTMR_CH_2);
    (void)app_gptmr_stop(APP_GPTMR_CH_3);
}
void app_motion_timer_get_snapshot(app_motion_timer_snapshot_t* out) {
    if (out != NULL) {
        uint32_t state = intf_sys_irq_save();
        *out = s_motion_timer_snapshot;
        intf_sys_irq_restore(state);
    }
}
void app_motion_timer_take_window(app_motion_timer_snapshot_t* out) {
    if (out != NULL) {
        uint32_t state = intf_sys_irq_save();
        *out = s_motion_timer_snapshot;
        s_motion_timer_snapshot.tick.window_count = 0;
        s_motion_timer_snapshot.tick.window_late_count = 0;
        s_motion_timer_snapshot.tick.period_min_cycles = 0;
        s_motion_timer_snapshot.tick.period_max_cycles = 0;
        s_motion_timer_snapshot.tick.execution_max_cycles = 0;
        s_motion_timer_snapshot.tick.feedback_age_min_cycles = 0;
        s_motion_timer_snapshot.tick.feedback_age_max_cycles = 0;
        s_motion_timer_snapshot.tick.window_slice_overrun_count = 0;
        intf_sys_irq_restore(state);
    }
}
