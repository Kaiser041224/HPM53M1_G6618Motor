/*
 * Encoder Interface - C17 抽象接口（设备对象 + 匿名结构体）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 语义约定：
 *   - read_raw：读取单圈绝对位置原始值（分辨率见 get_info），阻塞至完成；
 *     驱动内部固定小超时 + 错误计数，调用方只判 0/-1
 *   - read_reg / write_reg：寄存器访问（可选能力，不支持时返回 -1）
 *   - set_zero / set_direction：一次性配置，语义由各驱动定义，可能烧写器件 NVM
 *   - get_error_count：累计传输失败次数（单调递增）
 *   - 每实例单所有者：不可在多上下文并发调用
 */

#ifndef _INTF_ENCODER_H
#define _INTF_ENCODER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t intf_encoder_id_t;

typedef struct {
    uint8_t  bus;      /* 挂载总线（intf_spi bus） */
    uint32_t sclk_hz;  /* 期望 SCLK */
} intf_encoder_cfg_t;

typedef struct {
    uint8_t resolution_bits; /* 单圈分辨率位数 */
    bool    has_registers;   /* 是否支持寄存器读写 */
} intf_encoder_info_t;

/*
 * 编码器设备对象（风格 A：instance_id + 匿名结构体方法）
 * 用法：const intf_encoder_t *enc = intf_encoder_get(id); enc->read_raw(&raw);
 */
typedef struct {
    uint8_t instance_id;
    struct {
        int      (*init)(const intf_encoder_cfg_t *cfg);
        void     (*deinit)(void);
        int      (*read_raw)(uint16_t *raw);
        int      (*read_reg)(uint8_t addr, uint8_t *val);
        int      (*write_reg)(uint8_t addr, uint8_t val);
        int      (*set_zero)(uint16_t zero);
        int      (*set_direction)(bool cw_increasing);
        int      (*get_info)(intf_encoder_info_t *info);
        uint32_t (*get_error_count)(void);
    };
} intf_encoder_t;

int intf_encoder_register(const intf_encoder_t *dev);
const intf_encoder_t *intf_encoder_get(intf_encoder_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* _INTF_ENCODER_H */
