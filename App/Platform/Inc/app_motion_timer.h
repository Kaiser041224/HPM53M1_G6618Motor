/**
 * @file    app_motion_timer.h
 * @brief   Motion双周期编排平台（只读ADC缓存；可绑定速度实时回调）
 * @author  Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef APP_MOTION_TIMER_H
#define APP_MOTION_TIMER_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** @brief 入口周期与执行统计；不代表硬件触发延迟 */
typedef struct {
    uint32_t count;                /**< 启动以来累计回调数 */
    uint32_t window_count;         /**< 自上次take_window以来的回调数 */
    uint32_t period_min_cycles;    /**< 自上次take_window以来最短入口间隔 */
    uint32_t period_max_cycles;    /**< 自上次take_window以来最长入口间隔 */
    uint32_t execution_max_cycles; /**< 本窗回调最大耗时，不含PLIC/RTOS入口 */
    uint32_t late_count;           /**< 启动以来累计长间隔次数，不等于漏中断 */
    uint32_t window_late_count;    /**< 本报告窗口入口间隔>标称110%的次数 */
    uint32_t feedback_sequence;
    uint32_t feedback_age_cycles;
    bool feedback_valid;           /**< 转子快照有效；不是motion游标解算有效 */
} app_motion_timer_channel_snapshot_t;
/** @brief 两通道一致观测副本 */
typedef struct {
    app_motion_timer_channel_snapshot_t position;
    app_motion_timer_channel_snapshot_t velocity;
} app_motion_timer_snapshot_t;
/** @brief 配置GPTMR1 CH0=5kHz CH1=10kHz；调用前app_gptmr_init，配置后停止 */
int app_motion_timer_init(void);
/** @brief 启动纯5kHz/10kHz定时中断；不触发SPI，不等待PWM同步 */
int app_motion_timer_start(void);
/** @brief 停止期间绑定速度实时消费者；回调必须有界无RTOS调用 */
void app_motion_timer_bind_velocity(void (*callback)(void));
/** @brief 停止期间绑定位置实时消费者（CH0 5kHz拍）；回调必须有界无RTOS调用 */
void app_motion_timer_bind_position(void (*callback)(void));
/** @brief 停止双通道 */
void app_motion_timer_stop(void);
/** @brief 短临界区读取观测副本 */
void app_motion_timer_get_snapshot(app_motion_timer_snapshot_t* out);
/** @brief 单诊断消费者：复制清窗；累计count/late、上一入口时刻保留 */
void app_motion_timer_take_window(app_motion_timer_snapshot_t* out);
#ifdef __cplusplus
}
#endif
#endif
