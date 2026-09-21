/**
 * @file    intf_synt.h
 * @brief   SYNT 同步定时器抽象接口
 * @author  Kaiser
 *
 * SYNT 通过比较通道产生周期性同步事件，事件经 TRGM 路由以同步 PWM、GPTMR 等。
 * 不支持中断——纯硬件同步事件输出。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef INTF_SYNT_H
#define INTF_SYNT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SYNT 比较通道号
 */
typedef uint8_t intf_synt_ch_t;

/**
 * @brief SYNT 初始化配置
 */
typedef struct {
    uint32_t       reload_count;  /**< 重装载计数值 */
    intf_synt_ch_t cmp_channel;   /**< 比较通道号 */
    uint32_t       cmp_count;     /**< 比较计数值 */
} intf_synt_cfg_t;

/**
 * @brief SYNT 设备对象
 */
typedef struct {
    uint8_t instance_id; /**< 实例编号 */
    struct {
        /**
         * @brief 初始化 SYNT
         * @param cfg 初始化配置
         * @return 0 = 成功；-1 = 失败
         */
        int (*init)(const intf_synt_cfg_t *cfg);

        /**
         * @brief 启动计数
         * @return 0 = 成功；-1 = 失败
         */
        int (*start)(void);

        /**
         * @brief 停止计数
         * @return 0 = 成功；-1 = 失败
         */
        int (*stop)(void);

        /**
         * @brief 复位计数器
         * @return 0 = 成功；-1 = 失败
         */
        int (*reset)(void);

        /**
         * @brief 设置重装载计数值
         * @param reload_count 重装载计数值
         * @return 0 = 成功；-1 = 失败
         */
        int (*set_reload)(uint32_t reload_count);

        /**
         * @brief 设置比较通道计数值
         * @param ch 比较通道号
         * @param cmp_count 比较计数值
         * @return 0 = 成功；-1 = 失败
         */
        int (*set_compare)(intf_synt_ch_t ch, uint32_t cmp_count);

        /**
         * @brief 读取当前计数值
         * @return 当前计数值
         */
        uint32_t (*get_count)(void);
    };
} intf_synt_t;

/**
 * @brief 注册 SYNT 设备对象
 * @param ops 设备对象
 * @return 0 = 成功；-1 = 失败
 */
int intf_synt_register(const intf_synt_t *ops);

/**
 * @brief 初始化 SYNT
 * @param cfg 初始化配置
 * @return 0 = 成功；-1 = 失败
 */
int intf_synt_init(const intf_synt_cfg_t *cfg);

/**
 * @brief 启动计数
 * @return 0 = 成功；-1 = 失败
 */
int intf_synt_start(void);

/**
 * @brief 停止计数
 * @return 0 = 成功；-1 = 失败
 */
int intf_synt_stop(void);

/**
 * @brief 复位计数器
 * @return 0 = 成功；-1 = 失败
 */
int intf_synt_reset(void);

/**
 * @brief 设置重装载计数值
 * @param reload_count 重装载计数值
 * @return 0 = 成功；-1 = 失败
 */
int intf_synt_set_reload(uint32_t reload_count);

/**
 * @brief 设置比较通道计数值
 * @param ch 比较通道号
 * @param cmp_count 比较计数值
 * @return 0 = 成功；-1 = 失败
 */
int intf_synt_set_compare(intf_synt_ch_t ch, uint32_t cmp_count);

/**
 * @brief 读取当前计数值
 * @return 当前计数值；0 = 未注册
 */
uint32_t intf_synt_get_count(void);

#ifdef __cplusplus
}
#endif

#endif /* INTF_SYNT_H */
