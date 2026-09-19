/*
 * SPI Interface - C17 抽象接口（设备对象 + 匿名结构体）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
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
 */

#ifndef _INTF_SPI_H
#define _INTF_SPI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t intf_spi_bus_t; /* 总线实例号：0..3 -> SPI0..SPI3 */

typedef struct {
    uint32_t sclk_hz;   /* 期望 SCLK；驱动取可达的整数分频，失败返回 -1 */
    uint8_t  cpol;      /* 0 = 空闲低；1 = 空闲高 */
    uint8_t  cpha;      /* 0 = 首边沿采样；1 = 次边沿采样 */
    uint8_t  data_bits; /* 每帧位数（1..32） */
    uint8_t  cs_index;  /* 片选索引：0..3 -> CS0..CS3 */
} intf_spi_cfg_t;

/*
 * SPI 总线设备对象（风格 A：instance_id + 匿名结构体方法）
 * 用法：const intf_spi_t *spi = intf_spi_get(bus); spi->transfer(...);
 */
typedef struct {
    uint8_t instance_id;
    struct {
        int      (*init)(const intf_spi_cfg_t *cfg);
        int      (*transfer)(const void *tx, void *rx, size_t frames, uint32_t timeout_ms);
        void     (*deinit)(void);
        uint32_t (*get_sclk_hz)(void);
    };
} intf_spi_t;

int intf_spi_register(const intf_spi_t *dev);
const intf_spi_t *intf_spi_get(intf_spi_bus_t bus);

#ifdef __cplusplus
}
#endif

#endif /* _INTF_SPI_H */
