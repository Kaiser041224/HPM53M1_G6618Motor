/**
 * @file    app_gptmr.h
 * @brief   GPTMR 外环定时器平台封装
 * @author  Kaiser
 *
 * GPTMR1 外环定时器封装。
 * 实际频率与相位需运行态测量；同源时钟不等于锁相。
 * 通过 intf_gptmr 接口层访问驱动，不直接调用驱动层。
 *
 * 通道分配 (GPTMR1):
 *   CH0 — motion 单控制拍 15kHz（position → velocity）
 *   CH1 — 预留 15kHz（motion 速度独立拍已合并至 CH0）
 *   CH2 — 通用通道 10kHz
 *   CH3 — 预留（原 12.5kHz 编码器采样器，已由 ADC 同拍硬件同步采样替代）
 *
 * 注：GPTMR0 CH2（全局通道 2）**现无使用者**（原 ADC1 慢速序列触发链已于 2026-10-26
 * 移除，ADC1 SEQ 改由 PWM1_CH10REF 同源触发）→ 可复用。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_GPTMR_H
#define APP_GPTMR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief GPTMR 周期回调
 */
typedef void (*app_gptmr_callback_t)(void);

/**
 * @brief GPTMR 通道标识
 */
typedef enum {
    APP_GPTMR_CH_0 = 0, /**< GPTMR1 CH0: motion 单控制拍 */
    APP_GPTMR_CH_1 = 1, /**< GPTMR1 CH1: 预留（速度拍已合并至 CH0） */
    APP_GPTMR_CH_2 = 2, /**< GPTMR1 CH2: 通用通道 */
    APP_GPTMR_CH_3 = 3, /**< GPTMR1 CH3: 预留（原编码器采样器，已移除；全局 7） */
    APP_GPTMR_CH_COUNT,
} app_gptmr_ch_t;

/**
 * @brief 初始化 GPTMR 平台
 */
void app_gptmr_init(void);

/**
 * @brief 启动指定通道
 * @param ch 通道
 * @return 0 = 成功；-1 = 失败
 */
int app_gptmr_start(app_gptmr_ch_t ch);

/**
 * @brief 停止指定通道
 * @param ch 通道
 * @return 0 = 成功；-1 = 失败
 */
int app_gptmr_stop(app_gptmr_ch_t ch);

/**
 * @brief 启动全部通道
 */
void app_gptmr_start_all(void);

/**
 * @brief 停止全部通道
 */
void app_gptmr_stop_all(void);

/**
 * @brief 注册通道周期回调，保持已配置频率；重初始化后通道停止
 * @param ch 通道
 * @param cb 回调（NULL = 清除）
 * @return 0 = 成功；-1 = 失败
 */
int app_gptmr_register_callback(app_gptmr_ch_t ch, app_gptmr_callback_t cb);

/**
 * @brief 设置通道频率（驱动会启动计数器；配置后需要显式stop）
 * @param ch 通道
 * @param frequency_hz 频率 [Hz]
 * @return 0 = 成功；-1 = 失败
 */
int app_gptmr_set_frequency(app_gptmr_ch_t ch, uint32_t frequency_hz);

#ifdef __cplusplus
}
#endif

#endif /* APP_GPTMR_H */
