/**
 * @file    app_debug_inverter.c
 * @brief   三相逆变桥输出自检（PWM1 → 合封预驱）
 * @author  Kaiser
 *
 * 相映射（与板级 pinmux 一致）：
 *   U = PWM1 ch4/5（PA20/21 → HIN1/LIN1）
 *   V = PWM1 ch6/7（PA22/23 → HIN2/LIN2）
 *   W = PWM1 ch0/1（PA24/25 → HIN3/LIN3）
 *
 * 测试内容：三相默认频率（见 config/hardware.yaml）中心对齐、50% 占空比、持续输出（示波器检查
 * HO/LO）。 平台控制逻辑在 app_3phase_inverter（本文件仅自检/调试编排）。
 *
 * 硬件前提（台架）：
 *   - 母线建议低压限流（如 12~24V / 限流 0.2~0.5A），首次检查不接电机
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_debug_inverter.h"

#include "app_3phase_inverter.h"
#include "app_debug_rtt.h"
#include "app_hardware_params.h"

#include <stddef.h>

/* 测试开关：bring-up 阶段为 1；量产固件应置 0 */
#define INVERTER_TEST_ENABLE (1U)

#define INVERTER_TEST_DUTY     (0.5f)
#define INVERTER_TEST_MASK_ALL (0x07U)

void app_debug_inverter_set_output(uint8_t mask) {
    mask &= INVERTER_TEST_MASK_ALL;

    if (mask == 0U) {
        app_3phase_inverter_disable();
    } else {
        (void)app_3phase_inverter_enable();
        for (uint8_t i = 0U; i < (uint8_t)APP_3PHASE_COUNT; i++) {
            if ((mask & (uint8_t)(1U << i)) != 0U) {
                (void)app_3phase_inverter_release((app_3phase_id_t)i);
            } else {
                (void)app_3phase_inverter_force_low((app_3phase_id_t)i);
            }
        }
    }

    app_debug_printf(
        "[3PH] mask=0x%X [%s%s%s] %s\r\n", (unsigned)mask, ((mask & 1U) != 0U) ? "U" : "-",
        ((mask & 2U) != 0U) ? "V" : "-", ((mask & 4U) != 0U) ? "W" : "-",
        (mask != 0U) ? "ON" : "OFF");
}

void app_debug_inverter_init(void) {
#if INVERTER_TEST_ENABLE
    const app_hardware_params_t* hardware;

    hardware = app_hardware_params_current(); /* config/hardware.yaml */
    app_debug_printf(
        "\r\n[3PH] 三相逆变桥输出自检：%u Hz / %u%% / 持续\r\n",
        (unsigned)hardware->inverter.pwm_freq_hz, (unsigned)(INVERTER_TEST_DUTY * 100.0f));

    app_3phase_inverter_init(NULL);
    (void)app_3phase_inverter_set_duty_abc(
        INVERTER_TEST_DUTY, INVERTER_TEST_DUTY, INVERTER_TEST_DUTY);
    app_debug_inverter_set_output(INVERTER_TEST_MASK_ALL);
#else
    app_debug_printf("[3PH] test disabled\r\n");
#endif
}
