/**
 * @file    app_3phase_inverter.c
 * @brief   三相逆变桥平台封装
 * @author  Kaiser
 *
 * 开关频率选型（2026-09-19，依据 G66-18 参数 + HPM53M1 能力）：
 *   电机：10 极对、Ls=0.1185mH、Rs=0.158Ω、额定 7A、母线 48V、3300rpm
 *   1) 相电流纹波（最坏点 D≈0.5）：ΔIpp ≈ Vbus/(4·f·Ls) ≈ 101/f [A]
 *      20kHz→5.1A、25kHz→4.0A、40kHz→2.5A（对 7A 额定均可接受）
 *   2) 死区失真：内部预驱 50~250ns + HPM 侧 50ns；25kHz 下占比 <1%
 *   3) 可听噪声：>20kHz 不可闻（25kHz 留余量）
 *   4) 低侧采样窗口：中心对齐下溢点，Dmax=0.95 时仍 ~1µs（运放 10MHz 可稳定）
 *   5) CPU：编码器读 7µs + FOC 估算 ~5µs ≈ 30% @480MHz（编码器 25kHz 已实测）
 *   6) 一致性：M1 已实测验证的控制环/编码器仿真即为 25kHz
 *   → 取 25kHz 为默认；20kHz 为降损备选
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_3phase_inverter.h"

#include "app_gpio.h"
#include "app_hardware_params.h"
#include "app_hrpwm.h"
#include "app_ramfunc.h"
#include "intf_clock.h"

#include <stddef.h>

/* 相 → PWM 配对映射（板级，与 pinmux 一致） */
APP_ATTR_FAST_RAM_INIT
static const hrpwm_pair_t s_phase_pair[APP_3PHASE_COUNT] = {
    HRPWM_PAIR_C, /* U: PWM1 ch4/5（PA20/21） */
    HRPWM_PAIR_D, /* V: PWM1 ch6/7（PA22/23） */
    HRPWM_PAIR_E, /* W: PWM1 ch0/1（PA24/25） */
};

/* 零电压矢量占空比 */
#define APP_3PHASE_INVERTER_DUTY_ZERO (0.5f)

static bool s_enabled;
static bool s_gate_power_on;
static float s_duty[APP_3PHASE_COUNT];

#define APP_3PHASE_GATE_POWER_ON_DELAY_MS (20U) /**< +12V 上电稳定等待 [ms] */
#define APP_3PHASE_GATE_OFF_SETTLE_MS     (20U) /**< 关栅极供电后等待驱动无源 [ms] */

/**
 * @brief 将占空比限制到 [0, 1]；NaN 视为零电压矢量
 * @param duty 原始占空比
 * @return 限制后的占空比
 */
APP_ATTR_RAMFUNC
static float inverter_clamp_duty(float duty) {
    if (duty != duty) {                  /* NaN（异常计算）→ 零电压矢量，避免输出危险矢量 */
        return APP_3PHASE_INVERTER_DUTY_ZERO;
    }
    if (duty < 0.0f) {
        return 0.0f;
    }
    if (duty > 1.0f) {
        return 1.0f;
    }
    return duty;
}

void app_3phase_inverter_init(const app_3phase_inverter_cfg_t* cfg) {
    const app_hardware_params_t* hardware;
    app_3phase_inverter_cfg_t cfg_effective;

    hardware = app_hardware_params_current(); /* config/hardware.yaml（将来 flash 覆盖） */
    cfg_effective = (app_3phase_inverter_cfg_t){
        .pwm_freq_hz = hardware->inverter.pwm_freq_hz,
        .deadtime_ns = hardware->inverter.deadtime_ns,
    };

    if (cfg != NULL) {
        cfg_effective = *cfg;
    }

    app_hrpwm_init();                    /* 注册驱动 + 平台默认配对配置 */

    /* 按配置重配三相（频率/死区；来源 config/hardware.yaml） */
    for (uint8_t i = 0U; i < (uint8_t)APP_3PHASE_COUNT; i++) {
        (void)app_hrpwm_config_pair(
            s_phase_pair[i], cfg_effective.pwm_freq_hz, cfg_effective.deadtime_ns);
        s_duty[i] = APP_3PHASE_INVERTER_DUTY_ZERO;
        app_hrpwm_set_duty(s_phase_pair[i], s_duty[i]);
    }

    app_3phase_inverter_disable(); /* 输出与 +12V 均保持关闭 */
    s_enabled = false;
    s_gate_power_on = false;
}

int app_3phase_inverter_gate_power_on(void) {
    if (s_enabled) {
        return -1; /* 输出使能时不允许单独改栅极供电 */
    }
    if (s_gate_power_on) {
        return 0;
    }
    app_gpio_set(PIN_GDRV_12V_EN, 1U);
    intf_clock_delay_ms(APP_3PHASE_GATE_POWER_ON_DELAY_MS);
    s_gate_power_on = true;
    return 0;
}

int app_3phase_inverter_gate_power_off(void) {
    if (s_enabled) {
        return -1; /* 必须先停 PWM */
    }
    if (!s_gate_power_on) {
        return 0;
    }
    app_gpio_set(PIN_GDRV_12V_EN, 0U);
    s_gate_power_on = false;
    return 0;
}

bool app_3phase_inverter_gate_power_is_on(void) { return s_gate_power_on; }

int app_3phase_inverter_enable(void) {
    if (s_enabled) {
        return 0;
    }

    /* 安全顺序：先开 +12V 栅极供电，稳定后再启动三相 PWM。
     * 若 driver power on 已预先上电，则不再重复等待。 */
    if (app_3phase_inverter_gate_power_on() != 0) {
        return -1;
    }

    for (uint8_t i = 0U; i < (uint8_t)APP_3PHASE_COUNT; i++) {
        app_hrpwm_start(s_phase_pair[i]);
    }

    s_enabled = true;
    return 0;
}

/**
 * @brief 停止三相 PWM 输出（不改变 +12V 栅极供电）
 */
static void inverter_stop_outputs(void) {
    for (uint8_t i = 0U; i < (uint8_t)APP_3PHASE_COUNT; i++) {
        app_hrpwm_stop(s_phase_pair[i]);
    }
}

void app_3phase_inverter_coast(void) {
    /* 正常关闭：只停输出，保留 +12V。转子可 coast，避免零矢量主动短路；
     * 也避免关 +12V 栅极供电造成的母线/3.3V 瞬态。 */
    inverter_stop_outputs();
    s_enabled = false;
}

void app_3phase_inverter_disable_soft(void) {
    /* 正常停机的低瞬态顺序：
     *   1) 先撤 +12V 栅极供电 → 驱动输出进入无源态；
     *   2) 等驱动/栅极电荷释放；
     *   3) 最后停 PWM 输出。
     * 若先停 PWM、栅极驱动仍带电，实测会在停止瞬间造成 USB CDC 断连。
     * 故障/急停不调用本函数，仍走 disable/emergency_stop 的强制顺序。 */
    bool was_gate_on = s_gate_power_on;

    app_gpio_set(PIN_GDRV_12V_EN, 0U);
    s_gate_power_on = false;
    if (was_gate_on) {
        intf_clock_delay_ms(APP_3PHASE_GATE_OFF_SETTLE_MS);
    }
    inverter_stop_outputs();
    s_enabled = false;
}

void app_3phase_inverter_disable(void) {
    bool was_gate_on = s_gate_power_on;

    /* 全关：先停 PWM，等输出级/栅极电荷释放，再断 +12V。 */
    inverter_stop_outputs();
    if (was_gate_on) {
        intf_clock_delay_ms(5U);
    }
    app_gpio_set(PIN_GDRV_12V_EN, 0U);
    s_enabled = false;
    s_gate_power_on = false;
}

APP_ATTR_RAMFUNC /* 25kHz FOC ISR 输出点 */
int app_3phase_inverter_set_duty_abc(float duty_u, float duty_v, float duty_w) {
    const float duty_in[APP_3PHASE_COUNT] = {duty_u, duty_v, duty_w};

    for (uint8_t i = 0U; i < (uint8_t)APP_3PHASE_COUNT; i++) {
        float duty = inverter_clamp_duty(duty_in[i]);

        app_hrpwm_set_duty(s_phase_pair[i], duty);
        s_duty[i] = duty;
    }

    return 0;
}

int app_3phase_inverter_set_duty(app_3phase_id_t phase, float duty) {
    if (phase >= APP_3PHASE_COUNT) {
        return -1;
    }

    duty = inverter_clamp_duty(duty);
    app_hrpwm_set_duty(s_phase_pair[phase], duty);
    s_duty[phase] = duty;

    return 0;
}

int app_3phase_inverter_force_low(app_3phase_id_t phase) {
    if (phase >= APP_3PHASE_COUNT) {
        return -1;
    }

    app_hrpwm_force_low(s_phase_pair[phase]);
    return 0;
}

int app_3phase_inverter_release(app_3phase_id_t phase) {
    if (phase >= APP_3PHASE_COUNT) {
        return -1;
    }

    app_hrpwm_force_release(s_phase_pair[phase]);
    return 0;
}

APP_ATTR_RAMFUNC /* ISR 紧急关桥路径 */
void app_3phase_inverter_emergency_stop(void) {
    /* 先归零电压矢量，再强制关断，避免再次使能时输出残留静态矢量 */
    (void)app_3phase_inverter_set_duty_abc(
        APP_3PHASE_INVERTER_DUTY_ZERO, APP_3PHASE_INVERTER_DUTY_ZERO,
        APP_3PHASE_INVERTER_DUTY_ZERO);
    for (uint8_t i = 0U; i < (uint8_t)APP_3PHASE_COUNT; i++) {
        app_hrpwm_force_low(s_phase_pair[i]);
    }
    app_gpio_set(PIN_GDRV_12V_EN, 0U);
    s_enabled = false;
    s_gate_power_on = false;
}

bool app_3phase_inverter_is_enabled(void) { return s_enabled; }

void app_3phase_inverter_get_duty_abc(float* duty_u, float* duty_v, float* duty_w) {
    float* out[APP_3PHASE_COUNT] = {duty_u, duty_v, duty_w};

    for (uint8_t i = 0U; i < (uint8_t)APP_3PHASE_COUNT; i++) {
        if (out[i] != NULL) {
            *out[i] = s_duty[i];
        }
    }
}
