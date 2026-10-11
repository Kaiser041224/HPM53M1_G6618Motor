/**
 * @file    intf_spi.h
 * @brief   SPI 抽象接口（设备对象 + 匿名结构体）
 * @author  Kaiser
 *
 * 语义约定：
 *   - 一次 transfer = 一个片选（CS）周期；CS 由控制器硬件自动控制
 *   - 全双工：发送 frames 帧的同时接收 frames 帧；只写/只读请传 dummy 缓冲
 *   - cpol/cpha：0 = 空闲低 / 首边沿采样，1 = 空闲高 / 次边沿采样
 *     （mode0 = 0/0，mode3 = 1/1）
 *   - tx/rx 指向的每个元素大小 = data_bits/8 字节（16bit 帧 → uint16_t）
 *   - timeout_ms 语义（与 uart/can 一致）：
 *       0          = 不等待
 *       UINT32_MAX = 无限等待
 *       其他       = 毫秒级超时
 *   - 硬件触发单帧（trig_*，可选能力）：外部触发源经 DMA 写 SPI CMD 启动一帧，
 *     CPU 只在消费点 fetch 结果（零启动开销、零忙等）；帧格式沿用 init 配置。
 *     arm 后该总线归触发通道独占，不得再调用 transfer。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _INTF_SPI_H
#define _INTF_SPI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SPI 总线实例号
 */
typedef uint8_t intf_spi_bus_t; /* 总线实例号：0..3 -> SPI0..SPI3 */

/**
 * @brief SPI 总线配置
 */
typedef struct {
    uint32_t sclk_hz;   /**< 期望 SCLK；驱动取可达的整数分频，失败返回 -1 */
    uint8_t  cpol;      /**< 0 = 空闲低；1 = 空闲高 */
    uint8_t  cpha;      /**< 0 = 首边沿采样；1 = 次边沿采样 */
    uint8_t  data_bits; /**< 每帧位数（1..32） */
    uint8_t  cs_index;  /**< 片选索引：0..3 -> CS0..CS3 */
} intf_spi_cfg_t;

/**
 * @brief 硬件触发源（经触发矩阵转发的 DMA 请求号）
 */
typedef enum {
    INTF_SPI_TRIG_DMA_REQ0 = 0, /**< 触发矩阵 DMA 请求 0（见 intf_trgm INTF_TRGM_DST_DMA_REQ0） */
    INTF_SPI_TRIG_DMA_REQ1,     /**< 触发矩阵 DMA 请求 1（见 intf_trgm INTF_TRGM_DST_DMA_REQ1） */
} intf_spi_trig_src_t;

/**
 * @brief 硬件触发单帧配置
 */
typedef struct {
    intf_spi_trig_src_t src; /**< 触发源（每次触发启动一帧） */
    uint32_t tx_word;        /**< 每帧发送字（预填 TX FIFO，保持 MOSI 电平确定） */
} intf_spi_trig_cfg_t;

/*
 * SPI 总线设备对象（风格 A：instance_id + 匿名结构体方法）
 * 用法：const intf_spi_t *spi = intf_spi_get(bus); spi->transfer(...);
 */

/**
 * @brief SPI 总线设备对象
 */
typedef struct {
    uint8_t instance_id; /**< 实例编号 */
    struct {
        /**
         * @brief 初始化 SPI 总线
         * @param cfg 总线配置
         * @return 0 = 成功；-1 = 失败
         */
        int (*init)(const intf_spi_cfg_t *cfg);

        /**
         * @brief 全双工传输（一个 CS 周期）
         * @param tx 发送缓冲区（可为 NULL 表示只读）
         * @param rx 接收缓冲区（可为 NULL 表示只写）
         * @param frames 帧数
         * @param timeout_ms 超时（0=不等待，UINT32_MAX=无限，其他=毫秒）
         * @return 0 = 成功；-1 = 失败
         */
        int (*transfer)(const void *tx, void *rx, size_t frames, uint32_t timeout_ms);

        /**
         * @brief 反初始化 SPI 总线
         */
        void (*deinit)(void);

        /**
         * @brief 读取实际生效的 SCLK 频率
         * @return SCLK 频率 [Hz]
         */
        uint32_t (*get_sclk_hz)(void);

        /**
         * @brief 启用硬件触发单帧（可选能力；NULL = 不支持）
         * @param cfg 触发配置
         * @return 0 = 成功；-1 = 不支持/未初始化/资源失败
         * @note 调用前总线须空闲；arm 后 transfer 不可再用（单一所有者）
         */
        int (*trig_arm)(const intf_spi_trig_cfg_t *cfg);

        /**
         * @brief 取最近一次硬件触发帧的接收字（中断上下文，有界）
         * @param rx_word 输出接收字（取 RX FIFO 中最新一帧，旧帧丢弃）
         * @param wait_cycles 帧仍在传输时的最长等待 [CPU cycle]（0 = 不等待）
         * @return 0 = 取到新帧；-1 = 无新帧（触发丢失/未 arm）；-2 = 等待超时（帧仍在传输）
         * @note 每次调用同时补满 TX FIFO，保证后续触发不欠载
         */
        int (*trig_fetch)(uint32_t *rx_word, uint32_t wait_cycles);

        /**
         * @brief 关闭硬件触发（停止 DMA，清空 FIFO；之后 transfer 可用）
         */
        void (*trig_disarm)(void);
    };
} intf_spi_t;

/**
 * @brief 注册 SPI 总线设备对象
 * @param dev 设备对象
 * @return 0 = 成功；-1 = 失败
 */
int intf_spi_register(const intf_spi_t *dev);

/**
 * @brief 获取 SPI 总线设备对象
 * @param bus 总线实例号
 * @return 设备对象指针；NULL = 未注册/编号越界
 */
const intf_spi_t *intf_spi_get(intf_spi_bus_t bus);

#ifdef __cplusplus
}
#endif

#endif /* _INTF_SPI_H */
