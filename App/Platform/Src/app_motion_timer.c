/**
 * @file    app_motion_timer.c
 * @brief   Motion GPTMR编排，编码器只读ADC快照，速度回调可发布电流请求
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
static app_motion_timer_snapshot_t s_motion_timer_snapshot;
static uint32_t s_previous_cycle[2];
static uint32_t s_expected_cycles[2];
static bool s_motion_timer_initialized;
static void (*s_velocity_callback)(void);
static void (*s_position_callback)(void);
void app_motion_timer_bind_velocity(void (*callback)(void)) {
    uint32_t st = intf_sys_irq_save();
    s_velocity_callback = callback;
    intf_sys_irq_restore(st);
}
void app_motion_timer_bind_position(void (*callback)(void)) {
    uint32_t st = intf_sys_irq_save();
    s_position_callback = callback;
    intf_sys_irq_restore(st);
}
/** @brief 只观测最新编码器快照和入口间隔 */
APP_ATTR_RAMFUNC
static void motion_timer_observe(unsigned index, app_motion_timer_channel_snapshot_t* snapshot) {
    uint32_t start = intf_clock_get_cycle();
    if (snapshot->count != 0) {
        uint32_t period = start - s_previous_cycle[index];
        if (snapshot->period_min_cycles == 0 || period < snapshot->period_min_cycles)
            snapshot->period_min_cycles = period;
        if (period > snapshot->period_max_cycles)
            snapshot->period_max_cycles = period;
        if (period > s_expected_cycles[index] + s_expected_cycles[index] / 10U) {
            snapshot->late_count++;
            snapshot->window_late_count++;
        }
    }
    s_previous_cycle[index] = start;
    snapshot->count++;
    snapshot->window_count++;
    app_encoder_rotor_snapshot_t encoder;
    int rc = app_encoder_get_rotor_snapshot(&encoder);
    snapshot->feedback_valid = rc == 0 && encoder.valid;
    if (rc == 0) {
        snapshot->feedback_sequence = encoder.seq;
        snapshot->feedback_age_cycles = encoder.age_cycles;
    }
    uint32_t elapsed = intf_clock_get_cycle() - start;
    if (elapsed > snapshot->execution_max_cycles)
        snapshot->execution_max_cycles = elapsed;
}
/** @brief 10kHz位置入口：计时并执行绑定的外环回调（CH0在共享IRQ中先分发） */
APP_ATTR_RAMFUNC
static void motion_timer_position(void) {
    uint32_t start = intf_clock_get_cycle();
    motion_timer_observe(0, &s_motion_timer_snapshot.position);
    if (s_position_callback)
        s_position_callback();
    uint32_t elapsed = intf_clock_get_cycle() - start;
    if (elapsed > s_motion_timer_snapshot.position.execution_max_cycles)
        s_motion_timer_snapshot.position.execution_max_cycles = elapsed;
}
/** @brief 15kHz速度入口：计时并执行绑定的外环回调 */
APP_ATTR_RAMFUNC
static void motion_timer_velocity(void) {
    uint32_t start = intf_clock_get_cycle();
    motion_timer_observe(1, &s_motion_timer_snapshot.velocity);
    if (s_velocity_callback)
        s_velocity_callback();
    uint32_t elapsed = intf_clock_get_cycle() - start;
    if (elapsed > s_motion_timer_snapshot.velocity.execution_max_cycles)
        s_motion_timer_snapshot.velocity.execution_max_cycles = elapsed;
}
int app_motion_timer_init(void) {
    app_motion_timer_stop();
    s_motion_timer_initialized = false;
    s_motion_timer_snapshot = (app_motion_timer_snapshot_t){0};
    s_expected_cycles[0] = intf_clock_get_cpu_freq() / 10000U;
    s_expected_cycles[1] = intf_clock_get_cpu_freq() / 15000U;
    intf_gptmr_cfg_t position = {
        .mode = INTF_GPTMR_MODE_TIMER, .frequency_hz = 10000, .callback = motion_timer_position};
    intf_gptmr_cfg_t velocity = {
        .mode = INTF_GPTMR_MODE_TIMER, .frequency_hz = 15000, .callback = motion_timer_velocity};
    if (intf_gptmr_init(4, &position) != 0 || intf_gptmr_init(5, &velocity) != 0) {
        app_motion_timer_stop();
        return -1;
    }
    app_motion_timer_stop();
    s_motion_timer_initialized = true;
    return 0;
}
int app_motion_timer_start(void) {
    if (!s_motion_timer_initialized)
        return -1;
    app_motion_timer_stop();
    s_motion_timer_snapshot = (app_motion_timer_snapshot_t){0};
    if (app_gptmr_start(APP_GPTMR_CH_0) != 0 || app_gptmr_start(APP_GPTMR_CH_1) != 0) {
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
        s_motion_timer_snapshot.position.window_count = 0;
        s_motion_timer_snapshot.position.window_late_count = 0;
        s_motion_timer_snapshot.position.period_min_cycles = 0;
        s_motion_timer_snapshot.position.period_max_cycles = 0;
        s_motion_timer_snapshot.position.execution_max_cycles = 0;
        s_motion_timer_snapshot.velocity.window_count = 0;
        s_motion_timer_snapshot.velocity.window_late_count = 0;
        s_motion_timer_snapshot.velocity.period_min_cycles = 0;
        s_motion_timer_snapshot.velocity.period_max_cycles = 0;
        s_motion_timer_snapshot.velocity.execution_max_cycles = 0;
        intf_sys_irq_restore(state);
    }
}
