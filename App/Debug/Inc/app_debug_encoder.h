/**
 * @file    app_debug_encoder.h
 * @brief   编码器自检 + 25kHz 采样仿真（双 KTH7823）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_DEBUG_ENCODER_H
#define APP_DEBUG_ENCODER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Ozone 观测变量（定义于 app_debug_encoder.c，.noncacheable.bss 段，启动清零）：
 * sample 每个控制周期更新（25kHz 仿真下即 25kHz 刷新）；
 * 在 Ozone 的 Watch / Plot 窗口按符号名添加即可。
 */
extern volatile uint16_t g_enc_rotor_raw;      /* 转子编码器 16bit 原始值 */
extern volatile uint16_t g_enc_output_raw;     /* 出轴编码器 16bit 原始值 */
extern volatile float g_enc_rotor_deg;         /* 转子机械角 [deg]（零点修正后），0..360 */
extern volatile float g_enc_output_deg;        /* 出轴机械角 [deg]（零点修正后），0..360 */
extern volatile uint32_t g_enc_rotor_read_us;  /* 转子单次读耗时 [us] */
extern volatile uint32_t g_enc_output_read_us; /* 出轴单次读耗时 [us] */
extern volatile uint32_t g_enc_loop_late_us;   /* 最近一次节拍迟到 [us]，0 = 未迟到 */
extern volatile int32_t g_enc_ratio_x10000;    /* 游标比值累计 ×10000（1kHz 成对采样） */

/**
 * @brief 初始化编码器自检：注册驱动 + 初始化双路 + 在线/寄存器检查 + 耗时实测。
 */
void app_debug_encoder_init(void);

/**
 * @brief 转子编码器单次采样（25kHz 快车道 / ADC PMT ISR：FOC 电角度反馈）。
 */
void app_debug_encoder_sample_rotor(void);

/**
 * @brief 出轴编码器单次采样（1kHz 慢任务；角度闭环阶段随角度环频率提速）。
 */
void app_debug_encoder_sample_output(void);

/**
 * @brief 兼容包装：转子 + 出轴背靠背采样（调试/初始化路径用）。
 */
void app_debug_encoder_sample(void);

/**
 * @brief 主循环节拍迟到反馈（迟到周期数，由节拍点在错过期限时调用）。
 */
void app_debug_encoder_note_loop_late(uint32_t late_cycles);

/**
 * @brief 1Hz 汇总打印（读耗时 avg/max、实际速率、迟到统计、错误计数）。
 */
void app_debug_encoder_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_DEBUG_ENCODER_H */
