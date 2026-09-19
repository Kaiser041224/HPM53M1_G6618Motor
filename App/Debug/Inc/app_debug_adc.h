/*
 * Debug ADC - 采样链调试（配置打印 / 通道 dump / 诊断统计）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_DEBUG_ADC_H
#define APP_DEBUG_ADC_H

#include "app_adc.h"

/* ============================================================================
 * Ozone 观测变量（.noncacheable.bss：启动清零 + 调试器直读，不受 D-Cache 影响）
 * 每个 25kHz 控制周期由 app_debug_adc_update() 刷新
 * ============================================================================ */

extern volatile float    g_adc_i_u_a;      /* U 相电流 [A] */
extern volatile float    g_adc_i_v_a;      /* V 相电流 [A] */
extern volatile float    g_adc_i_w_a;      /* W 相电流 [A] */
extern volatile float    g_adc_v_bus_v;    /* 母线电压 [V] */
extern volatile float    g_adc_r_ntc0_ohm; /* NTC0 电阻 [Ω] */
extern volatile float    g_adc_r_ntc1_ohm; /* NTC1 电阻 [Ω] */
extern volatile uint16_t g_adc_raw[ADC_CH_COUNT]; /* 原始码（顺序同 adc_channel_t） */
extern volatile uint32_t g_adc_sequence;   /* 电流帧序号（判断新数据） */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 25kHz 周期调用：刷新 Ozone 观测变量（不打印） */
void app_debug_adc_update(void);

/** @brief 打印采样链配置（启动时调用） */
void app_debug_adc_init(void);

/** @brief 命令 d：全通道表（raw / mV / 物理量） */
void app_debug_adc_dump_channels(void);

/** @brief 命令 p：诊断统计（ISR / PMT 完成率 / 非法帧 / ISR 周期） */
void app_debug_adc_dump_diag(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_DEBUG_ADC_H */
