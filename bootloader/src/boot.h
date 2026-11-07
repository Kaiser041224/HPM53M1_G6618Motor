/**
 * @file    boot.h
 * @brief   Bootloader 启动决策、槽校验与跳转
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BOOT_BOOT_H
#define BOOT_BOOT_H

#include "intf_boot.h"

#include <stdbool.h>
#include <stdint.h>

/** 新镜像连续启动失败次数上限；超过后回滚到 active 槽 */
#define BOOT_MAX_ATTEMPTS (3U)

typedef enum {
    BOOT_DECISION_APP = 0, /**< 校验通过，跳转 *slot_out */
    BOOT_DECISION_DFU = 1, /**< 无可用镜像（或强制请求），进入 DFU */
} boot_decision_t;

/** @brief 加载 flash 后端（必须在时钟初始化之后调用一次） */
void boot_init(void);

/**
 * @brief 启动决策：pending 优先、失败回滚、active 兜底、都失败进 DFU
 * @param slot_out 决策为 BOOT_DECISION_APP 时返回目标槽
 * @param force_dfu true = 直接进 DFU（邮箱请求）
 */
boot_decision_t boot_decide(uint8_t *slot_out, bool force_dfu);

/** @brief 校验槽：镜像日志不是 FLASHING + 头合法 + payload CRC 一致 */
bool boot_slot_valid(uint8_t slot_id);

/** @brief DFU 开始写入：追加 FLASHING 记录 */
int boot_slot_mark_flashing(uint8_t slot_id, const boot_image_header_t *hdr);

/** @brief DFU 写入完成：追加 READY 记录 + 启动控制写 pending */
int boot_slot_commit(uint8_t slot_id, const boot_image_header_t *hdr, uint32_t image_crc32);

/** @brief 把控制记录清成 active 不变、pending=NONE（回滚） */
int boot_clear_pending(void);

/**
 * @brief 跳转到槽入口（关中断 + 清目标槽 I-cache + 跳 槽基址+entry_offset）
 * @note 不返回
 */
void boot_jump(uint8_t slot_id) __attribute__((noreturn));

#endif /* BOOT_BOOT_H */
