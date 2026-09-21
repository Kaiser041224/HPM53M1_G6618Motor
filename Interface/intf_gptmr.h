/**
 * @file    intf_gptmr.h
 * @brief   通用定时器（GPTMR）抽象接口
 * @author  Kaiser
 *
 * 支持每通道独立配置：
 *   - PWM 输出（占空比、频率）
 *   - 周期定时中断（回调）
 *   - PWM 输出 + 中断（组合）
 *   - 输入捕获（边沿检测）
 *   - SYNCI 同步（经 TRGM 由 SYNT 触发）
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef INTF_GPTMR_H
#define INTF_GPTMR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief GPTMR 通道号
 */
typedef uint8_t intf_gptmr_ch_t;

/**
 * @brief GPTMR 中断回调（中断上下文执行）
 */
typedef void (*intf_gptmr_irq_callback_t)(void);

/**
 * @brief GPTMR 通道工作模式
 */
typedef enum {
    INTF_GPTMR_MODE_PWM       = 0, /**< 仅 PWM 输出 */
    INTF_GPTMR_MODE_TIMER     = 1, /**< 仅周期定时中断 */
    INTF_GPTMR_MODE_PWM_TIMER = 2, /**< PWM 输出 + 中断 */
    INTF_GPTMR_MODE_CAPTURE   = 3, /**< 输入捕获 */
} intf_gptmr_mode_t;

/**
 * @brief GPTMR 捕获边沿
 */
typedef enum {
    INTF_GPTMR_CAPTURE_EDGE_RISING  = 0, /**< 上升沿 */
    INTF_GPTMR_CAPTURE_EDGE_FALLING = 1, /**< 下降沿 */
    INTF_GPTMR_CAPTURE_EDGE_BOTH    = 2, /**< 双边沿 */
} intf_gptmr_capture_edge_t;

/**
 * @brief GPTMR 通道配置
 */
typedef struct {
    intf_gptmr_mode_t         mode;         /**< 工作模式 */
    uint32_t                  frequency_hz; /**< 频率 [Hz] */
    float                     duty;         /**< PWM 模式占空比 [0.0, 1.0] */
    bool                      invert_output;/**< PWM 模式：输出反相 */
    intf_gptmr_capture_edge_t capture_edge; /**< CAPTURE 模式捕获边沿 */
    intf_gptmr_irq_callback_t callback;     /**< TIMER / PWM_TIMER 模式回调 */
    bool                      enable_sync;  /**< 使能 SYNCI（SYNT 同步） */
} intf_gptmr_cfg_t;

/**
 * @brief GPTMR 捕获结果
 */
typedef struct {
    bool     captured;     /**< 本次是否捕获到 */
    uint32_t count;        /**< 捕获计数值 */
    uint32_t period_ticks; /**< 捕获周期 [tick] */
} intf_gptmr_capture_t;

/**
 * @brief GPTMR 抽象接口
 */
typedef struct {
    uint8_t instance_id; /**< 实例编号 */
    struct {
        /**
         * @brief 初始化通道
         * @param ch 通道号
         * @param cfg 通道配置
         * @return 0 = 成功；-1 = 失败
         */
        int (*init)(intf_gptmr_ch_t ch, const intf_gptmr_cfg_t *cfg);

        /**
         * @brief 启动通道
         * @param ch 通道号
         * @return 0 = 成功；-1 = 失败
         */
        int (*start)(intf_gptmr_ch_t ch);

        /**
         * @brief 停止通道
         * @param ch 通道号
         * @return 0 = 成功；-1 = 失败
         */
        int (*stop)(intf_gptmr_ch_t ch);

        /**
         * @brief 设置占空比
         * @param ch 通道号
         * @param duty 占空比 [0.0, 1.0]
         * @return 0 = 成功；-1 = 失败
         */
        int (*set_duty)(intf_gptmr_ch_t ch, float duty);

        /**
         * @brief 设置频率
         * @param ch 通道号
         * @param frequency_hz 频率 [Hz]
         * @return 0 = 成功；-1 = 失败
         */
        int (*set_frequency)(intf_gptmr_ch_t ch, uint32_t frequency_hz);

        /**
         * @brief 强制输出低电平
         * @param ch 通道号
         * @return 0 = 成功；-1 = 失败
         */
        int (*force_low)(intf_gptmr_ch_t ch);

        /**
         * @brief 解除强制低电平
         * @param ch 通道号
         * @return 0 = 成功；-1 = 失败
         */
        int (*force_release)(intf_gptmr_ch_t ch);

        /**
         * @brief 轮询读取捕获结果
         * @param ch 通道号
         * @param capture 输出捕获结果
         * @return 0 = 成功；-1 = 失败
         */
        int (*capture_poll)(intf_gptmr_ch_t ch, intf_gptmr_capture_t *capture);
    };
} intf_gptmr_t;

/**
 * @brief 注册 GPTMR 接口实现
 * @param ops 接口实现
 * @return 0 = 成功；-1 = 失败
 */
int intf_gptmr_register(const intf_gptmr_t *ops);

/**
 * @brief 初始化通道
 * @param ch 通道号
 * @param cfg 通道配置
 * @return 0 = 成功；-1 = 失败
 */
int intf_gptmr_init(intf_gptmr_ch_t ch, const intf_gptmr_cfg_t *cfg);

/**
 * @brief 启动通道
 * @param ch 通道号
 * @return 0 = 成功；-1 = 失败
 */
int intf_gptmr_start(intf_gptmr_ch_t ch);

/**
 * @brief 停止通道
 * @param ch 通道号
 * @return 0 = 成功；-1 = 失败
 */
int intf_gptmr_stop(intf_gptmr_ch_t ch);

/**
 * @brief 设置占空比
 * @param ch 通道号
 * @param duty 占空比 [0.0, 1.0]
 * @return 0 = 成功；-1 = 失败
 */
int intf_gptmr_set_duty(intf_gptmr_ch_t ch, float duty);

/**
 * @brief 设置频率
 * @param ch 通道号
 * @param frequency_hz 频率 [Hz]
 * @return 0 = 成功；-1 = 失败
 */
int intf_gptmr_set_frequency(intf_gptmr_ch_t ch, uint32_t frequency_hz);

/**
 * @brief 强制输出低电平
 * @param ch 通道号
 * @return 0 = 成功；-1 = 失败
 */
int intf_gptmr_force_low(intf_gptmr_ch_t ch);

/**
 * @brief 解除强制低电平
 * @param ch 通道号
 * @return 0 = 成功；-1 = 失败
 */
int intf_gptmr_force_release(intf_gptmr_ch_t ch);

/**
 * @brief 轮询读取捕获结果
 * @param ch 通道号
 * @param capture 输出捕获结果
 * @return 0 = 成功；-1 = 失败
 */
int intf_gptmr_capture_poll(intf_gptmr_ch_t ch, intf_gptmr_capture_t *capture);

#ifdef __cplusplus
}
#endif

#endif /* INTF_GPTMR_H */
