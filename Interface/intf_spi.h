/*
 * SPI Interface - C17 Abstract Interface
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
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t intf_spi_bus_t;

typedef struct {
    uint32_t sclk_hz;   /* 期望 SCLK；驱动取可达的整数分频，失败返回 -1 */
    uint8_t  cpol;      /* 0 = 空闲低；1 = 空闲高 */
    uint8_t  cpha;      /* 0 = 首边沿采样；1 = 次边沿采样 */
    uint8_t  data_bits; /* 每帧位数（1..32） */
    uint8_t  cs_index;  /* 片选索引：0..3 -> CS0..CS3 */
} intf_spi_cfg_t;

typedef struct {
    int      (*init)(intf_spi_bus_t bus, const intf_spi_cfg_t *cfg);
    int      (*transfer)(intf_spi_bus_t bus, const void *tx, void *rx,
                         size_t frames, uint32_t timeout_ms);
    void     (*deinit)(intf_spi_bus_t bus);
    uint32_t (*get_sclk_hz)(intf_spi_bus_t bus);
} intf_spi_ops_t;

int      intf_spi_register(const intf_spi_ops_t *ops);
int      intf_spi_init(intf_spi_bus_t bus, const intf_spi_cfg_t *cfg);
int      intf_spi_transfer(intf_spi_bus_t bus, const void *tx, void *rx,
                           size_t frames, uint32_t timeout_ms);
void     intf_spi_deinit(intf_spi_bus_t bus);
uint32_t intf_spi_get_sclk_hz(intf_spi_bus_t bus);

#ifdef __cplusplus
}
#endif

#endif /* _INTF_SPI_H */
