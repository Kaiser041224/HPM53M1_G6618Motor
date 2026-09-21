/**
 * @file    intf_gpio.h
 * @brief   GPIO 抽象接口（多实例）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef INTF_GPIO_H
#define INTF_GPIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * @brief GPIO 引脚号
 */
typedef uint16_t intf_gpio_pin_t;

/**
 * @brief GPIO 方向
 */
typedef enum {
    INTF_GPIO_DIR_INPUT  = 0, /**< 输入 */
    INTF_GPIO_DIR_OUTPUT = 1, /**< 输出 */
} intf_gpio_dir_t;

/**
 * @brief GPIO 电平
 */
typedef enum {
    INTF_GPIO_LEVEL_LOW  = 0, /**< 低电平 */
    INTF_GPIO_LEVEL_HIGH = 1, /**< 高电平 */
} intf_gpio_level_t;

/**
 * @brief GPIO 上下拉配置
 */
typedef enum {
    INTF_GPIO_PULL_NONE = 0, /**< 无上下拉 */
    INTF_GPIO_PULL_DOWN = 1, /**< 下拉 */
    INTF_GPIO_PULL_UP   = 2, /**< 上拉 */
} intf_gpio_pull_t;

/**
 * @brief GPIO 中断触发方式
 */
typedef enum {
    INTF_GPIO_IRQ_NONE         = 0, /**< 不触发中断 */
    INTF_GPIO_IRQ_EDGE_RISING  = 1, /**< 上升沿 */
    INTF_GPIO_IRQ_EDGE_FALLING = 2, /**< 下降沿 */
    INTF_GPIO_IRQ_EDGE_BOTH    = 3, /**< 双边沿 */
    INTF_GPIO_IRQ_LEVEL_HIGH   = 4, /**< 高电平 */
    INTF_GPIO_IRQ_LEVEL_LOW    = 5, /**< 低电平 */
} intf_gpio_irq_mode_t;

/**
 * @brief GPIO 中断回调（中断上下文执行）
 * @param pin 触发引脚
 * @param user_data 用户数据
 */
typedef void (*intf_gpio_irq_cb_t)(intf_gpio_pin_t pin, void *user_data);

/**
 * @brief GPIO 引脚配置
 */
typedef struct {
    intf_gpio_pin_t      pin;           /**< 引脚号 */
    intf_gpio_dir_t      dir;           /**< 方向 */
    intf_gpio_pull_t     pull;          /**< 上下拉 */
    intf_gpio_level_t    init_level;    /**< 初始输出电平 */
    intf_gpio_irq_mode_t irq_mode;      /**< 中断触发方式 */
    intf_gpio_irq_cb_t   irq_cb;        /**< 中断回调（可为 NULL） */
    void                *irq_user_data; /**< 中断回调用户数据 */
} intf_gpio_cfg_t;

/* ============================================================================
 * Interface Definition (Object-Oriented C17)
 * ============================================================================ */

/**
 * @brief GPIO 抽象接口
 */
typedef struct {
    /**
     * @brief 初始化引脚
     * @param cfg 引脚配置
     * @return 0 = 成功；-1 = 失败
     */
    int (*init)(const intf_gpio_cfg_t *cfg);

    /**
     * @brief 设置输出电平
     * @param pin 引脚号
     * @param level 输出电平
     * @return 0 = 成功；-1 = 失败
     */
    int (*set_level)(intf_gpio_pin_t pin, intf_gpio_level_t level);

    /**
     * @brief 读取输入电平
     * @param pin 引脚号
     * @param level 输出电平
     * @return 0 = 成功；-1 = 失败
     */
    int (*get_level)(intf_gpio_pin_t pin, intf_gpio_level_t *level);

    /**
     * @brief 翻转输出电平
     * @param pin 引脚号
     * @return 0 = 成功；-1 = 失败
     */
    int (*toggle)(intf_gpio_pin_t pin);
} intf_gpio_t;

/* ============================================================================
 * Registration API
 * ============================================================================ */

/**
 * @brief 注册 GPIO 接口实现
 * @param ops 接口实现
 * @return 0 = 成功；-1 = 失败
 */
int intf_gpio_register(const intf_gpio_t *ops);

/* ============================================================================
 * Functional API (wraps ops)
 * ============================================================================ */

/**
 * @brief 初始化引脚
 * @param cfg 引脚配置
 * @return 0 = 成功；-1 = 失败
 */
int intf_gpio_init(const intf_gpio_cfg_t *cfg);

/**
 * @brief 设置输出电平
 * @param pin 引脚号
 * @param level 输出电平
 * @return 0 = 成功；-1 = 失败
 */
int intf_gpio_set_level(intf_gpio_pin_t pin, intf_gpio_level_t level);

/**
 * @brief 读取输入电平
 * @param pin 引脚号
 * @param level 输出电平
 * @return 0 = 成功；-1 = 失败
 */
int intf_gpio_get_level(intf_gpio_pin_t pin, intf_gpio_level_t *level);

/**
 * @brief 翻转输出电平
 * @param pin 引脚号
 * @return 0 = 成功；-1 = 失败
 */
int intf_gpio_toggle(intf_gpio_pin_t pin);

#ifdef __cplusplus
}
#endif

#endif /* INTF_GPIO_H */
