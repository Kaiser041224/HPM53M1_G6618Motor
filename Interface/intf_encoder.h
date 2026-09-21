/**
 * @file    intf_encoder.h
 * @brief   编码器抽象接口（设备对象 + 匿名结构体）
 * @author  Kaiser
 *
 * 语义约定：
 *   - read_raw：读取单圈绝对位置原始值（分辨率见 get_info），阻塞至完成；
 *     驱动内部固定小超时 + 错误计数，调用方只判 0/-1
 *   - read_reg / write_reg：寄存器访问（可选能力，不支持时返回 -1）
 *   - set_zero / set_direction：一次性配置，语义由各驱动定义，可能烧写器件 NVM
 *   - get_error_count：累计传输失败次数（单调递增）
 *   - 每实例单所有者：不可在多上下文并发调用
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _INTF_ENCODER_H
#define _INTF_ENCODER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 编码器实例编号
 */
typedef uint8_t intf_encoder_id_t;

/**
 * @brief 编码器初始化配置
 */
typedef struct {
    uint8_t  bus;     /**< 挂载总线（intf_spi bus） */
    uint32_t sclk_hz; /**< 期望 SCLK [Hz] */
} intf_encoder_cfg_t;

/**
 * @brief 编码器能力信息
 */
typedef struct {
    uint8_t resolution_bits; /**< 单圈分辨率位数 */
    bool    has_registers;   /**< 是否支持寄存器读写 */
} intf_encoder_info_t;

/*
 * 编码器设备对象（风格 A：instance_id + 匿名结构体方法）
 * 用法：const intf_encoder_t *enc = intf_encoder_get(id); enc->read_raw(&raw);
 */

/**
 * @brief 编码器设备对象
 */
typedef struct {
    uint8_t instance_id; /**< 实例编号 */
    struct {
        /**
         * @brief 初始化编码器
         * @param cfg 初始化配置
         * @return 0 = 成功；-1 = 失败
         */
        int (*init)(const intf_encoder_cfg_t *cfg);

        /**
         * @brief 反初始化编码器
         */
        void (*deinit)(void);

        /**
         * @brief 读取单圈绝对位置原始值（阻塞）
         * @param raw 输出原始位置
         * @return 0 = 成功；-1 = 失败
         */
        int (*read_raw)(uint16_t *raw);

        /**
         * @brief 读取寄存器（可选能力）
         * @param addr 寄存器地址
         * @param val 输出寄存器值
         * @return 0 = 成功；-1 = 不支持/失败
         */
        int (*read_reg)(uint8_t addr, uint8_t *val);

        /**
         * @brief 写入寄存器（可选能力）
         * @param addr 寄存器地址
         * @param val 寄存器值
         * @return 0 = 成功；-1 = 不支持/失败
         */
        int (*write_reg)(uint8_t addr, uint8_t val);

        /**
         * @brief 设置零点
         * @param zero 零点原始值
         * @return 0 = 成功；-1 = 失败
         */
        int (*set_zero)(uint16_t zero);

        /**
         * @brief 设置计数方向
         * @param cw_increasing true = 顺时针递增
         * @return 0 = 成功；-1 = 失败
         */
        int (*set_direction)(bool cw_increasing);

        /**
         * @brief 读取编码器能力信息
         * @param info 输出信息
         * @return 0 = 成功；-1 = 失败
         */
        int (*get_info)(intf_encoder_info_t *info);

        /**
         * @brief 读取累计传输失败次数
         * @return 失败次数（单调递增）
         */
        uint32_t (*get_error_count)(void);
    };
} intf_encoder_t;

/**
 * @brief 注册编码器设备对象
 * @param dev 设备对象
 * @return 0 = 成功；-1 = 失败
 */
int intf_encoder_register(const intf_encoder_t *dev);

/**
 * @brief 获取编码器设备对象
 * @param id 实例编号
 * @return 设备对象指针；NULL = 未注册/编号越界
 */
const intf_encoder_t *intf_encoder_get(intf_encoder_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* _INTF_ENCODER_H */
