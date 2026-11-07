/**
 * @file    app_status_led.h
 * @brief   状态 LED 灯语（RTOS 任务内推进，不进入 ISR）
 * @author  Kaiser
 *
 * 灯语状态（优先级从高到低）：
 *   FAULT（SOS）> CALIB（双闪）> RUN（按节点 ID 的脉冲）> IDLE（2Hz 方波）
 *
 * 时间基准为 1ms 拍（app_io_step 的 1kHz 调用），全部在 io 任务内完成；
 * 灯语核心为纯函数，主机侧可直接喂时间戳断言波形。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_STATUS_LED_H
#define APP_STATUS_LED_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 灯语使用的 FOC 状态（与 app_foc_state_t 解耦，便于主机测试） */
typedef enum {
    APP_STATUS_LED_FOC_OFF = 0, /**< 输出关闭 */
    APP_STATUS_LED_FOC_READY,   /**< 桥已使能，零矢量 */
    APP_STATUS_LED_FOC_RUN,     /**< 电流闭环 */
    APP_STATUS_LED_FOC_CALIB,   /**< 辨识模式 */
    APP_STATUS_LED_FOC_FAULT,   /**< 故障门控 */
} app_status_led_foc_t;

/** @brief 灯语输入快照 */
typedef struct {
    app_status_led_foc_t foc; /**< FOC 状态 */
    bool fault_latched;       /**< 故障状态机处于 FAULT（policy=TRIP） */
    uint8_t node_id;          /**< 节点号 0..7；无效时忽略 */
    bool node_id_valid;       /**< node_id 是否有效 */
} app_status_led_input_t;

/** @brief 灯语运行上下文（模式 + 模式起始时间） */
typedef struct {
    uint8_t mode;         /**< 当前灯语模式（内部枚举） */
    uint32_t mode_start_ms; /**< 当前模式起始时间 [ms] */
} app_status_led_ctx_t;

/**
 * @brief 初始化状态 LED（PB01 输出、初始灭）
 */
void app_status_led_init(void);

/**
 * @brief 推进灯语（1kHz io 任务内调用）
 * @param now_ms 当前毫秒时基（FreeRTOS tick，1ms）
 */
void app_status_led_step(uint32_t now_ms);

/**
 * @brief 重置灯语上下文（测试/重新初始化用）
 * @param ctx 上下文
 * @param now_ms 起始时间 [ms]
 */
void app_status_led_ctx_init(app_status_led_ctx_t* ctx, uint32_t now_ms);

/**
 * @brief 灯语核心：根据输入与时间计算 LED 亮灭
 * @param ctx 上下文（模式切换时内部重置相位）
 * @param in 输入快照
 * @param now_ms 当前时间 [ms]
 * @return true = 点亮；false = 熄灭
 */
bool app_status_led_eval(app_status_led_ctx_t* ctx, const app_status_led_input_t* in,
                         uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* APP_STATUS_LED_H */
