/**
 * @file    dfu.h
 * @brief   Bootloader DFU 模式（TinyUSB DFU 1.1 + WCID 免驱）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BOOT_DFU_H
#define BOOT_DFU_H

#include <stdbool.h>
#include <stdint.h>

/** DFU 空闲超时 [ms]：主机在此时长内未发起任何传输则回到 active 槽 */
#define BOOT_DFU_IDLE_TIMEOUT_MS (60000U)

/**
 * @brief 进入 DFU 模式并运行（不返回；manifest 成功或超时后软复位）
 * @param target_slot 允许写入的目标槽（非活动槽）
 * @param allow_timeout true = 存在可回退的 active 槽，空闲超时后复位回应用
 */
void dfu_run(uint8_t target_slot, bool allow_timeout) __attribute__((noreturn));

/** @brief 本次 DFU 的目标槽（0=ACTIVE, 1=BACKUP）；用于描述符里的接口名 */
uint8_t dfu_target_slot(void);

#endif /* BOOT_DFU_H */
