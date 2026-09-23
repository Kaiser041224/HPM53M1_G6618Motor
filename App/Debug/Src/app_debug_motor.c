/**
 * @file    app_debug_motor.c
 * @brief   电机开环旋转自检（V/F）
 * @author  Kaiser
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
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_debug_motor.h"

#include "algo_trig.h"
#include "app_3phase_inverter.h"
#include "app_adc.h"
#include "app_debug_rtt.h"
#include "app_hardware_params.h"
#include "intf_clock.h"

#include <stdbool.h>

#define MOTOR_TWO_PI    (6.283185307179586f)
#define MOTOR_SQ3_2     (0.8660254037844386f) /* √3/2，用于 sin(θ±120°) 恒等式 */

#define MOTOR_TEST_FREQ_DEFAULT (1.0f)
#define MOTOR_TEST_FREQ_MIN     (0.5f)
#define MOTOR_TEST_FREQ_MAX     (10.0f)
#define MOTOR_TEST_FREQ_STEP    (0.5f)

#define MOTOR_TEST_MOD_DEFAULT (0.03f)        /* 3%：24V 母线时相电流约 2.3A */
#define MOTOR_TEST_MOD_MIN     (0.01f)
#define MOTOR_TEST_MOD_MAX     (0.10f)
#define MOTOR_TEST_MOD_STEP    (0.01f)

static bool s_running;
static float s_freq_hz;
static float s_mod;
/* 角度状态（[rad]，25kHz 硬触发节拍下 dt 恒定 = 1/pwm_freq）。
 * sin/cos 由 algo_trig_sin_cos 查表得出（HPM SDK hpm_mcl/bldc_foc_sin_cos
 * 惯例：四象限折叠 + 单表镜像，一次查表出 sin/cos，无浮点三角函数、无漂移）。 */
static float s_angle_rad;
static float s_delta_rad;

/**
 * @brief 重算角度步长（频率变更/启动时调用，任务上下文）
 */
static void motor_angle_recompute_delta(void) {
    const app_hardware_params_t* hardware = app_hardware_params_current();

    s_delta_rad = MOTOR_TWO_PI * s_freq_hz / (float)hardware->inverter.pwm_freq_hz;
}

bool app_debug_motor_is_running(void) {
    return s_running;
}

void app_debug_motor_get_state(float* freq_hz, float* mod, bool* running) {
    if (freq_hz != NULL) {
        *freq_hz = s_freq_hz;
    }
    if (mod != NULL) {
        *mod = s_mod;
    }
    if (running != NULL) {
        *running = s_running;
    }
}

/**
 * @brief 打印当前旋转状态（启停 / 电频率 / 调制比）
 */
static void motor_print_state(void) {
    app_debug_printf(
        "[MOTOR] rotation=%s f=%.2f Hz mod=%.1f%% (Vamp=%.2f V @24V)\r\n", s_running ? "ON" : "OFF",
        (double)s_freq_hz, (double)(s_mod * 100.0f), (double)(s_mod * 24.0f * 0.5f));
}

void app_debug_motor_set_freq(float freq_hz) {
    if (freq_hz < MOTOR_TEST_FREQ_MIN) {
        freq_hz = MOTOR_TEST_FREQ_MIN;
    } else if (freq_hz > MOTOR_TEST_FREQ_MAX) {
        freq_hz = MOTOR_TEST_FREQ_MAX;
    }

    s_freq_hz = freq_hz;
    motor_angle_recompute_delta();
    motor_print_state();
}

void app_debug_motor_set_mod(float mod) {
    if (mod < MOTOR_TEST_MOD_MIN) {
        mod = MOTOR_TEST_MOD_MIN;
    } else if (mod > MOTOR_TEST_MOD_MAX) {
        mod = MOTOR_TEST_MOD_MAX;
    }

    s_mod = mod;
    motor_print_state();
}

/**
 * @brief 按旋转矢量角的 sin/cos 输出三相占空比（SPWM）
 *
 * sin(θ∓120°) = -0.5·sinθ ∓ (√3/2)·cosθ —— 单对 sin/cos 生成三相，
 * 与 hpm_mcl 的 inv_park(ud,uq,sin,cos) 同构（sin/cos 为入参，此处直接出三相）。
 * @param s sin(θ)
 * @param c cos(θ)
 */
static void motor_apply_sincos(float s, float c) {
    float half_mod = s_mod * 0.5f;

    (void)app_3phase_inverter_set_duty_abc(
        0.5f + half_mod * s, 0.5f + half_mod * (-0.5f * s - MOTOR_SQ3_2 * c),
        0.5f + half_mod * (-0.5f * s + MOTOR_SQ3_2 * c));
}

void app_debug_motor_init(void) {
    s_running = false;
    s_freq_hz = MOTOR_TEST_FREQ_DEFAULT;
    s_mod = MOTOR_TEST_MOD_DEFAULT;
    s_angle_rad = 0.0f;
    motor_angle_recompute_delta();

    app_debug_printf("[MOTOR] open-loop V/F ready (r=start/stop, +/-=freq, m/M=mod)\r\n");
    motor_print_state();
}

void app_debug_motor_run_once(void) {
    float s;
    float c;

    if (!s_running) {
        return;
    }

    /* 角度推进（25kHz 硬触发，Δ 恒定）+ 查表 sin/cos（SDK FOC 惯例） */
    s_angle_rad += s_delta_rad;
    if (s_angle_rad >= MOTOR_TWO_PI) {
        s_angle_rad -= MOTOR_TWO_PI;
    }
    algo_trig_sin_cos(s_angle_rad, &s, &c);
    motor_apply_sincos(s, c);
}

void app_debug_motor_rotation_toggle(void) {
    if (s_running) {
        app_debug_motor_stop();
        app_debug_printf("[MOTOR] rotation STOP\r\n");
        return;
    }

    /* 防御：占空比开始每周期更新前，重新武装 ADC 触发比较器（on_modify 单次写生效），
     * 避免 PWM 影子寄存器交互导致触发点被扰动（曾观测到 228kHz 触发突发）。 */
    {
        const app_hardware_params_t* hardware = app_hardware_params_current(); /* config/hardware.yaml */

        (void)app_adc_set_trigger_delay_ns(hardware->adc.trigger_delay_ns);
    }

    (void)app_3phase_inverter_enable();
    s_angle_rad = 0.0f;
    motor_angle_recompute_delta();
    s_running = true;
    {
        float s;
        float c;

        algo_trig_sin_cos(s_angle_rad, &s, &c);
        motor_apply_sincos(s, c);
    }
    app_debug_printf("[MOTOR] rotation START\r\n");
    motor_print_state();
}

void app_debug_motor_freq_step(int8_t dir) {
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

    motor_angle_recompute_delta();
    motor_print_state();
}

void app_debug_motor_mod_step(int8_t dir) {
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

void app_debug_motor_stop(void) {
    s_running = false;

    /* 先归零电压矢量（0.5/0.5/0.5）再关断，避免残留静态矢量在再次使能时
       产生直流电流 */
    (void)app_3phase_inverter_set_duty_abc(0.5f, 0.5f, 0.5f);
    app_3phase_inverter_disable();
}
