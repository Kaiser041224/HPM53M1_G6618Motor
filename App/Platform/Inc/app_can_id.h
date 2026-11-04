/**
 * @file    app_can_id.h
 * @brief   CAN 节点号：CANID 电阻网络原始码解码与上电一次读取
 * @author  Kaiser
 *
 * 硬件：ADC1_IN15 / PB00 采样 CANID 分压网络（上拉选择 + R78 分压），
 * 8 档（S1/S2/S3）。**ID = S1<<2 | S2<<1 | S3（拨码按二进制）**：
 * 000→0、001→1、010→2、011→3、100→4、101→5、110→6、111→7。
 * 阈值表为电压低→高的台架实测值，解码时经 level→ID 置换表映射，见 AGENTS.md。
 *
 * 使用纪律：
 *   - `app_can_id_read()` 只在上电初始化阶段调用一次（ADC 就绪后、CAN 服务初始化前）；
 *   - 读完立即 `app_adc_canid_release()` 把该通道从 ADC1 序列移除，运行期不再采样；
 *   - 解码结果写 RAM（调用方持有），运行期不再变化。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_CAN_ID_H
#define APP_CAN_ID_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 无效/无法解码的节点号 */
#define APP_CAN_ID_INVALID (0xFFU)

/**
 * @brief 原始码 → 档位（纯函数）
 *
 * @param raw 16 位 ADC 原始码（CANID 通道）
 * @return 0..7（拨码二进制 S1 S2 S3）；原始码越界/无数据返回 APP_CAN_ID_INVALID
 */
uint8_t app_can_id_decode(uint16_t raw);

/**
 * @brief 上电读取一次：多次采样 → 中值 → 离散度过滤 → 解码
 *
 * 采样期间会调用 `app_adc_slow_process()` 并间隔 1 ms，最多 9 次；
 * 有效样本不足 5 个或极差超过 512 LSB 时判失败。耗时约 9 ms，只允许 boot 调用。
 *
 * @return 0..7；失败返回 APP_CAN_ID_INVALID
 */
uint8_t app_can_id_read(void);

/** @brief 上电解码使用的原始码中值（0xFFFF = 无有效样本；诊断用） */
uint16_t app_can_id_get_boot_raw(void);

/** @brief 上电解码结果（0..7 或 APP_CAN_ID_INVALID；诊断用） */
uint8_t app_can_id_get_boot_id(void);

/** @brief 上电采样内点的极差 [LSB]（0xFFFF = 无有效样本；诊断用） */
uint16_t app_can_id_get_boot_spread(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CAN_ID_H */
