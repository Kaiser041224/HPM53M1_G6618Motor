/*
 * Debug Inverter - 三相逆变桥输出自检（PWM1 → 合封预驱）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_DEBUG_INVERTER_H
#define APP_DEBUG_INVERTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 三相逆变桥输出自检：25kHz / 50% 持续输出（示波器检查 HO/LO）。
 *
 * 平台控制逻辑见 App/Platform/app_3phase_inverter（本文件仅自检/调试编排）。
 * 安全：输出使能由 app_3phase_inverter 管理（+12V → PWM 的安全顺序）。
 */
void app_debug_inverter_init(void);

/**
 * @brief 逐相输出控制（用于故障定位与相序确认）。
 * @param mask 位掩码：bit0=U、bit1=V、bit2=W；0 = 全关（含 12V 关）
 */
void app_debug_inverter_set_output(uint8_t mask);

#ifdef __cplusplus
}
#endif

#endif /* APP_DEBUG_INVERTER_H */
