/*
 * Debug Motor - 电机开环旋转自检（V/F）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 目的：在不依赖电流采样/闭环的前提下，验证"逆变桥 + 电机 + 编码器"完整链路。
 *
 * 方法：旋转电压矢量（SPWM）
 *   θ += 2π·f_elec·dt（dt 用 mcycle 实测，抗主循环抖动）
 *   duty_x = 0.5 + (m/2)·sin(θ + 相位偏移)
 * 三相相位偏移 0 / −120° / +120°。
 *
 * 电流估算（低速时 ωL 可忽略，I ≈ V_amp/R_phase）：
 *   V_amp = m × Vbus/2；G66-18 R_phase = 0.158Ω
 *   例：Vbus=24V、m=3% → V_amp=0.36V → I≈2.3A；m=1% → I≈0.8A
 *
 * 安全：
 *   - 启动前需逆变桥使能（本模块自动调用，含 +12V 顺序）
 *   - 停止/急停即关输出 + 关 12V；命令 '0' 亦会停止旋转
 *   - 建议限流电源 + 低压起步（m 从小到大）
 */

#include "app_debug_motor.h"

#include "app_3phase_inverter.h"
#include "app_debug_rtt.h"
#include "intf_clock.h"

#include <math.h>
#include <stdbool.h>

#define MOTOR_TWO_PI       (6.283185307179586f)
#define MOTOR_PHASE_120    (2.0943951023931953f) /* 2π/3 */

#define MOTOR_TEST_FREQ_DEFAULT (1.0f)
#define MOTOR_TEST_FREQ_MIN     (0.5f)
#define MOTOR_TEST_FREQ_MAX     (10.0f)
#define MOTOR_TEST_FREQ_STEP    (0.5f)

#define MOTOR_TEST_MOD_DEFAULT  (0.03f) /* 3%：24V 母线时相电流约 2.3A */
#define MOTOR_TEST_MOD_MIN      (0.01f)
#define MOTOR_TEST_MOD_MAX      (0.10f)
#define MOTOR_TEST_MOD_STEP     (0.01f)

static bool     s_running;
static float    s_freq_hz;
static float    s_mod;
static float    s_theta;
static uint32_t s_last_cycle;

static void motor_print_state(void)
{
    app_debug_printf("[MOTOR] rotation=%s f=%.2f Hz mod=%.1f%% (Vamp=%.2f V @24V)\r\n",
                     s_running ? "ON" : "OFF", (double) s_freq_hz, (double) (s_mod * 100.0f),
                     (double) (s_mod * 24.0f * 0.5f));
}

/* 按当前角度输出三相占空比（θ 弧度） */
static void motor_apply(float theta)
{
    float half_mod = s_mod * 0.5f;

    (void) app_3phase_inverter_set_duty_abc(0.5f + half_mod * sinf(theta),
                                            0.5f + half_mod * sinf(theta - MOTOR_PHASE_120),
                                            0.5f + half_mod * sinf(theta + MOTOR_PHASE_120));
}

void app_debug_motor_init(void)
{
    s_running = false;
    s_freq_hz = MOTOR_TEST_FREQ_DEFAULT;
    s_mod = MOTOR_TEST_MOD_DEFAULT;
    s_theta = 0.0f;
    s_last_cycle = 0U;

    app_debug_printf("[MOTOR] open-loop V/F ready (r=start/stop, +/-=freq, m/M=mod)\r\n");
    motor_print_state();
}

void app_debug_motor_run_once(void)
{
    uint32_t now;
    uint32_t dt_cycles;
    float dt_s;

    if (!s_running) {
        return;
    }

    now = intf_clock_get_cycle();
    dt_cycles = now - s_last_cycle;
    s_last_cycle = now;

    dt_s = (float) dt_cycles / (float) intf_clock_get_cpu_freq();
    s_theta += MOTOR_TWO_PI * s_freq_hz * dt_s;
    if (s_theta >= MOTOR_TWO_PI) {
        s_theta -= MOTOR_TWO_PI;
    }

    motor_apply(s_theta);
}

void app_debug_motor_rotation_toggle(void)
{
    if (s_running) {
        app_debug_motor_stop();
        app_debug_printf("[MOTOR] rotation STOP\r\n");
        return;
    }

    (void) app_3phase_inverter_enable();
    s_theta = 0.0f;
    s_last_cycle = intf_clock_get_cycle();
    s_running = true;
    motor_apply(s_theta);
    app_debug_printf("[MOTOR] rotation START\r\n");
    motor_print_state();
}

void app_debug_motor_freq_step(int8_t dir)
{
    if (dir > 0) {
        s_freq_hz += MOTOR_TEST_FREQ_STEP;
    } else {
        s_freq_hz -= MOTOR_TEST_FREQ_STEP;
    }

    if (s_freq_hz < MOTOR_TEST_FREQ_MIN) {
        s_freq_hz = MOTOR_TEST_FREQ_MIN;
    }
    if (s_freq_hz > MOTOR_TEST_FREQ_MAX) {
        s_freq_hz = MOTOR_TEST_FREQ_MAX;
    }

    motor_print_state();
}

void app_debug_motor_mod_step(int8_t dir)
{
    if (dir > 0) {
        s_mod += MOTOR_TEST_MOD_STEP;
    } else {
        s_mod -= MOTOR_TEST_MOD_STEP;
    }

    if (s_mod < MOTOR_TEST_MOD_MIN) {
        s_mod = MOTOR_TEST_MOD_MIN;
    }
    if (s_mod > MOTOR_TEST_MOD_MAX) {
        s_mod = MOTOR_TEST_MOD_MAX;
    }

    motor_print_state();
}

void app_debug_motor_stop(void)
{
    s_running = false;

    /* 先归零电压矢量（0.5/0.5/0.5）再关断，避免残留静态矢量在再次使能时
       产生直流电流 */
    (void) app_3phase_inverter_set_duty_abc(0.5f, 0.5f, 0.5f);
    app_3phase_inverter_disable();
}
