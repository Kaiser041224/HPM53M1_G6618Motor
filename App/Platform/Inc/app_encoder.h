/*
 * App Encoder - 编码器平台封装（双 KTH7823）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 板级映射：
 *   APP_ENCODER_ROTOR  -> SPI3（PA10-13），转子 1:1
 *   APP_ENCODER_OUTPUT -> SPI1（PA26-29），出轴 49:50（游标）
 *
 * 实时性：read_raw 为阻塞短操作（标称 ~5µs），无打印/动态分配；
 *         每实例单所有者，不可在多上下文并发调用。
 */

#ifndef APP_ENCODER_H
#define APP_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_ENCODER_ROTOR = 0, /* 转子编码器：SPI3 */
    APP_ENCODER_OUTPUT,    /* 出轴编码器：SPI1 */
    APP_ENCODER_COUNT
} app_encoder_id_t;

/**
 * @brief 注册编码器驱动并初始化双路（KTH7823，mode3，10MHz）。
 * @return 0 = 全部成功；-1 = 存在失败（可查错误计数/寄存器读进一步诊断）
 */
int app_encoder_init(void);

/**
 * @brief 读取单圈绝对位置原始值（16bit 原码）。
 * @return 0 成功，-1 失败（累计于 get_error_count）
 */
int app_encoder_read_raw(app_encoder_id_t id, uint16_t *raw);

/**
 * @brief 读取机械角，单位 rad，范围 [0, 2π)。
 * @return 0 成功，-1 失败
 */
int app_encoder_read_rad(app_encoder_id_t id, float *rad);

/**
 * @brief 读取机械角，单位 deg，范围 [0, 360)。
 * @return 0 成功，-1 失败
 */
int app_encoder_read_deg(app_encoder_id_t id, float *deg);

/**
 * @brief 读寄存器（诊断；如 0x09 = RD，出厂默认 1）。
 * @return 0 成功，-1 失败
 */
int app_encoder_read_reg(app_encoder_id_t id, uint8_t addr, uint8_t *val);

/*
 * 一次性配置（写 MTP，器件寿命 1000 次；禁止运行期/周期调用）：
 */

/**
 * @brief 写零点 Z(15:0)（0x00/0x01，两次 MTP 写）。
 * @return 0 成功，-1 失败
 */
int app_encoder_set_zero(app_encoder_id_t id, uint16_t zero);

/**
 * @brief 写旋转方向 RD（0x09，一次 MTP 写）；true = 顺时针角度增加（出厂默认）。
 * @return 0 成功，-1 失败
 */
int app_encoder_set_direction(app_encoder_id_t id, bool cw_increasing);

/**
 * @brief 累计传输失败次数（单调递增）。
 */
uint32_t app_encoder_get_error_count(app_encoder_id_t id);

/**
 * @brief 当前实际 SCLK（Hz），0 = 未初始化。
 */
uint32_t app_encoder_get_sclk_hz(app_encoder_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* APP_ENCODER_H */
