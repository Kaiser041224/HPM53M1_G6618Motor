/*
 * Encoder Interface - C17 Abstract Interface
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 语义约定：
 *   - read_raw：读取单圈绝对位置原始值（分辨率见 get_info），阻塞至完成；
 *     驱动内部固定小超时 + 错误计数，调用方只判 0/-1
 *   - read_reg / write_reg：寄存器访问（可选能力，不支持时返回 -1）
 *   - set_zero / set_direction：一次性配置，可能烧写器件 NVM（见各驱动注释）
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

typedef struct {
    int      (*init)(intf_encoder_id_t id, const intf_encoder_cfg_t *cfg);
    void     (*deinit)(intf_encoder_id_t id);
    int      (*read_raw)(intf_encoder_id_t id, uint16_t *raw);
    int      (*read_reg)(intf_encoder_id_t id, uint8_t addr, uint8_t *val);
    int      (*write_reg)(intf_encoder_id_t id, uint8_t addr, uint8_t val);
    int      (*set_zero)(intf_encoder_id_t id, uint16_t zero);
    int      (*set_direction)(intf_encoder_id_t id, bool cw_increasing);
    int      (*get_info)(intf_encoder_id_t id, intf_encoder_info_t *info);
    uint32_t (*get_error_count)(intf_encoder_id_t id);
} intf_encoder_ops_t;

int      intf_encoder_register(const intf_encoder_ops_t *ops);
int      intf_encoder_init(intf_encoder_id_t id, const intf_encoder_cfg_t *cfg);
void     intf_encoder_deinit(intf_encoder_id_t id);
int      intf_encoder_read_raw(intf_encoder_id_t id, uint16_t *raw);
int      intf_encoder_read_reg(intf_encoder_id_t id, uint8_t addr, uint8_t *val);
int      intf_encoder_write_reg(intf_encoder_id_t id, uint8_t addr, uint8_t val);
int      intf_encoder_set_zero(intf_encoder_id_t id, uint16_t zero);
int      intf_encoder_set_direction(intf_encoder_id_t id, bool cw_increasing);
int      intf_encoder_get_info(intf_encoder_id_t id, intf_encoder_info_t *info);
uint32_t intf_encoder_get_error_count(intf_encoder_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* _INTF_ENCODER_H */
