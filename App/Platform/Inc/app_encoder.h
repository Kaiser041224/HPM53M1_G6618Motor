/**
 * @file    app_encoder.h
 * @brief   编码器平台封装（双 KTH7823）
 * @author  Kaiser
 *
 * 板级映射：
 *   APP_ENCODER_ROTOR  -> SPI3（PA10-13），转子 1:1
 *   APP_ENCODER_OUTPUT -> SPI1（PA26-29），出轴 49:50（游标）
 *
 * 实时性：read_raw 为阻塞短操作（标称 ~5µs），无打印/动态分配；
 *         每实例单所有者，不可在多上下文并发调用。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_ENCODER_H
#define APP_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 编码器实例标识
 */
typedef enum {
    APP_ENCODER_ROTOR = 0, /**< 转子编码器：SPI3 */
    APP_ENCODER_OUTPUT,    /**< 出轴编码器：SPI1 */
    APP_ENCODER_COUNT
} app_encoder_id_t;

/**
 * @brief 注册编码器驱动并初始化双路（KTH7823，mode3，10MHz）。
 * @return 0 = 全部成功；-1 = 存在失败（可查错误计数/寄存器读进一步诊断）
 */
int app_encoder_init(void);

/**
 * @brief 读取单圈绝对位置原始值（16bit 原码，未修正）。
 * @return 0 成功，-1 失败（累计于 get_error_count）
 */
int app_encoder_read_raw(app_encoder_id_t id, uint16_t* raw);

/**
 * @brief 转子编码器共享采样（25kHz 节拍单次读取并缓存）。
 *        供 FOC（Control）与 Debug 共用，避免重复 SPI 读（每次 ≈5~7µs）。
 * @return 0 = 成功；-1 = 失败（缓存保持上次值，valid 置 false）
 */
int app_encoder_sample_rotor(void);

/**
 * @brief 读取转子共享采样缓存（无 I/O）。
 * @param raw 输出原始值（未加软件零点）
 * @param valid 输出缓存有效性
 * @param seq 输出采样序号（每次成功采样 +1；可为 NULL）。
 *            消费方（如 FOC）应比对相邻节拍序号是否推进，以检测采样停摆。
 * @return 0 = 成功；-1 = 参数错误
 * @note 采样所有者：25kHz 节拍调用 app_encoder_sample_rotor() 的一方
 *       （当前为 app_debug_encoder_sample()）；消费方只读缓存。
 */
int app_encoder_get_rotor_raw(uint16_t* raw, bool* valid, uint32_t* seq);

/**
 * @brief 读取零点修正后的单圈位置：(raw − zero) & 0xFFFF。
 * @return 0 成功，-1 失败
 */
int app_encoder_read_position(app_encoder_id_t id, uint16_t* pos);

/**
 * @brief 读取机械角，单位 rad，范围 [0, 2π)。
 * @return 0 成功，-1 失败
 */
int app_encoder_read_rad(app_encoder_id_t id, float* rad);

/**
 * @brief 读取机械角，单位 deg，范围 [0, 360)。
 * @return 0 成功，-1 失败
 */
int app_encoder_read_deg(app_encoder_id_t id, float* deg);

/**
 * @brief 读寄存器（诊断；如 0x09 = RD，出厂默认 1）。
 * @return 0 成功，-1 失败
 */
int app_encoder_read_reg(app_encoder_id_t id, uint8_t addr, uint8_t* val);

/*
 * 零点（软件方案，推荐）：
 *   记录当前原始值到 flash 参数区（app_param），掉电保持，
 *   不消耗编码器 MTP（Z 寄存器寿命仅 1000 次写）。
 */

/**
 * @brief 软件设置零点：记录当前原始值并保存到 flash。
 *        之后 read_position / read_rad / read_deg 均以此为基准。
 * @return 0 成功，-1 失败
 */
int app_encoder_set_zero(app_encoder_id_t id);

/**
 * @brief 清除软件零点（偏移归零并写回 flash）。
 * @return 0 成功，-1 失败
 */
int app_encoder_clear_zero(app_encoder_id_t id);

/**
 * @brief 读取当前软件零点偏移。
 * @return 0 成功，-1 失败
 */
int app_encoder_get_zero(app_encoder_id_t id, uint16_t* zero);

/**
 * @brief 是否已从 flash 加载到有效的编码器参数记录（false = 使用默认值）。
 */
bool app_encoder_is_param_loaded(void);

/*
 * 硬件零点（写编码器 MTP，器件寿命 1000 次）——仅产线一次性标定用，慎调。
 */

/**
 * @brief 写编码器 Z(15:0) 寄存器（0x00/0x01，两次 MTP 写）。
 * @return 0 成功，-1 失败
 */
int app_encoder_set_zero_mtp(app_encoder_id_t id, uint16_t zero);

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
