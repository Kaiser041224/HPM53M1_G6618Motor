/**
 * @file    app_fault.h
 * @brief   故障保护与错误处理（v1：纯判断）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_FAULT_H
#define APP_FAULT_H

#include "app_adc.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 故障码（32 位位图；bit10-31 预留：温度等）
 * ============================================================================ */
#define APP_FAULT_NONE        (0x00000000UL)
#define APP_FAULT_OC_FAST_U   (1UL << 0) /* L1：ADC WDOG 硬件阈值（µs 级） */
#define APP_FAULT_OC_FAST_V   (1UL << 1)
#define APP_FAULT_OC_FAST_W   (1UL << 2)
#define APP_FAULT_OC_SLOW_U   (1UL << 3) /* L2：RMS 连续越限（1kHz 去抖） */
#define APP_FAULT_OC_SLOW_V   (1UL << 4)
#define APP_FAULT_OC_SLOW_W   (1UL << 5)
#define APP_FAULT_VBUS_OV     (1UL << 6) /* L3：母线过压 */
#define APP_FAULT_VBUS_UV     (1UL << 7) /* L3：母线欠压 */
#define APP_FAULT_ADC_TIMEOUT (1UL << 8) /* 健康：PMT 帧序号停滞 */
#define APP_FAULT_ENC_READ    (1UL << 9) /* 健康：编码器错误计数增量 */

/* ============================================================================
 * 状态机
 * ============================================================================ */
/**
 * @brief 故障状态机状态
 */
typedef enum {
    APP_FAULT_STATE_INIT = 0, /**< 初始化 */
    APP_FAULT_STATE_NORMAL,   /**< 正常 */
    APP_FAULT_STATE_WARNING,  /**< 去抖计数中（条件已出现，未达次数） */
    APP_FAULT_STATE_FAULT,    /**< 已触发（锁存；v1 无动作） */
} app_fault_state_t;

/* ============================================================================
 * 阈值默认值来源：config/software.yaml（app_software_params.fault，app_fault_init 加载）
 * 以下为内部时序量（待 FOC 时序体系定义后接入 YAML，见参数管线设计 §10）
 * ============================================================================ */
#define APP_FAULT_RMS_WINDOW_DEFAULT \
    (250U) /* 10ms @25kHz（TBD：FOC 时序体系接入后随 pwm_freq 派生） */
#define APP_FAULT_SETTLE_TICKS_DEFAULT (50U) /* 上电静默期（≈50ms @1kHz） */

/**
 * @brief 故障阈值配置（0 = 使用 app_software_params.fault 默认值）
 */
typedef struct {
    float oc_fast_a;        /**< L1 相电流阈值 [A]，0 = 默认 72.9A */
    float oc_slow_a;        /**< L2 RMS 阈值 [A]，0 = 默认 72.9A */
    float vbus_ov_v;        /**< 过压 [V]，0 = 默认 36V */
    float vbus_uv_v;        /**< 欠压 [V]，0 = 默认 9V */
    uint16_t slow_debounce; /**< L2/L3 去抖次数，0 = 默认 5 */
    uint16_t adc_stall_ms;  /**< PMT 帧超时 [ms]，0 = 默认 10 */
    uint8_t enc_err_delta;  /**< 编码器错误增量阈值，0 = 默认 3 */
} app_fault_cfg_t;

/**
 * @brief 首故障快照（进入 FAULT 时记录一次）
 */
typedef struct {
    uint32_t code;        /**< 首故障码（单个位） */
    float i_u_a;          /**< U 相电流 [A] */
    float i_v_a;          /**< V 相电流 [A] */
    float i_w_a;          /**< W 相电流 [A] */
    float v_bus_v;        /**< 母线电压 [V] */
    uint16_t oc_fast_raw; /**< L1 触发时的原始码（0 = 非 L1） */
} app_fault_snapshot_t;

/* ============================================================================
 * API
 * ============================================================================ */

/**
 * @brief 初始化（须在 app_adc_init 之前调用：提供 WDOG 阈值与回调）
 * @param cfg 故障阈值配置；NULL = 全默认
 */
void app_fault_init(const app_fault_cfg_t* cfg);

/**
 * @brief 25kHz 主循环：三相电流 RMS 累加
 */
void app_fault_process(void);

/**
 * @brief 1kHz（ADC1 序列完成回调，ISR 上下文；驱动保证每帧一次）：
 *        L2/L3/健康判断 + 执行清除请求
 */
void app_fault_tick(void);

/**
 * @brief L1：ADC0 WDOG 回调（ISR 上下文）
 * @param ch 逻辑通道
 * @param value WDOG 触发时的原始码
 * @param user 用户上下文（未使用）
 */
void app_fault_on_wdog(adc_channel_t ch, uint16_t value, void* user);

/**
 * @brief 清除锁存（条件须已恢复；完整复位——状态/锁存/首故障/快照/事件计数）。
 *        内部使用全局中断临界区，与故障 ISR 互斥。
 * @return 0 = 已清除；-1 = 条件未恢复/未初始化
 */
int app_fault_clear(void);

/**
 * @brief 当前故障状态机状态
 * @return 当前状态
 */
app_fault_state_t app_fault_get_state(void);

/**
 * @brief 当前存在的条件位图（条件恢复后自动解除；ENC_READ 为事件型仅经清除解除）
 * @return 当前条件位图
 */
uint32_t app_fault_get_codes(void);

/**
 * @brief 锁存故障位图（条件消失亦保持，直至清除）
 * @return 锁存故障位图
 */
uint32_t app_fault_get_latched(void);

/**
 * @brief 首故障码
 * @return 首故障码（单个位；0 = 无）
 */
uint32_t app_fault_get_first(void);

/**
 * @brief 各故障事件计数（按 bit0..；自上次清除以来）
 * @param out 输出数组
 * @param n 输出数组长度
 */
void app_fault_get_event_counts(uint16_t* out, uint8_t n);

/**
 * @brief 读取首故障快照
 * @param out 输出快照
 * @return true = 快照有效
 */
bool app_fault_get_snapshot(app_fault_snapshot_t* out);

/**
 * @brief 取 WDOG 原始阈值窗口（供 app_adc 配置；init 后有效）
 * @param thshd_high 高阈值输出
 * @param thshd_low 低阈值输出
 */
void app_fault_get_wdog_raw(uint16_t* thshd_high, uint16_t* thshd_low);

#ifdef __cplusplus
}
#endif

#endif /* APP_FAULT_H */
