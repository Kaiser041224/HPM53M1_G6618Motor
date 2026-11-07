/**
 * @file    app_boot.h
 * @brief   应用侧 OTA/DFU 支持：启动确认、槽信息、进入 DFU
 * @author  Kaiser
 *
 * 应用与 bootloader 的契约见 Interface/intf_boot.h：
 *   - 槽构建（-DAPP_SLOT_ID=0/1）在启动 2s 后确认 pending，写 active；
 *   - 进入 DFU = 写 SYSCTL GPR 邮箱 + 软复位，bootloader 消费后枚举 DFU；
 *   - 无 bootloader 的旧构建（未定义 APP_SLOT_ID）所有接口退化为空操作。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef APP_BOOT_H
#define APP_BOOT_H

#include "intf_boot.h"

#include <stdbool.h>
#include <stdint.h>

/** 本固件是否运行在 active/backup 槽构建下（= 定义了 APP_SLOT_ID） */
bool app_boot_is_slot_build(void);

/** 运行槽号（0=ACTIVE, 1=BACKUP）；非槽构建返回 0xFF */
uint8_t app_boot_slot_id(void);

/** 运行槽镜像版本号（镜像头 image_version）；非槽构建或无有效头返回 0 */
uint32_t app_boot_image_version(void);

/** 运行槽镜像头的 payload CRC32；非槽构建或无有效头返回 0 */
uint32_t app_boot_image_crc(void);

/**
 * @brief 自检：运行槽的头合法且 payload CRC 与头一致
 * @return 0 = OK；-1 = 非槽构建/头非法；-2 = payload CRC 不符
 * @note 读取整个槽 payload（约 25–30ms），仅在 FOC OFF 时由终端调用
 */
int app_boot_self_verify(void);

typedef struct {
    bool control_valid;   /**< flash 中存在有效启动控制记录 */
    uint8_t active_slot;  /**< 0=ACTIVE, 1=BACKUP */
    uint8_t pending_slot; /**< 0=ACTIVE, 1=BACKUP, 0xFF=无 */
    uint8_t attempts;     /**< pending 已尝试启动次数 */
    uint8_t confirmed;    /**< active 已由应用确认 */
    uint8_t running_slot; /**< 0=ACTIVE, 1=BACKUP, 0xFF=非槽构建 */
    uint8_t upgrade_target_slot; /**< bootloader 的 DFU 目标槽（= active ^ 1） */
    bool upgrade_target_valid;   /**< 目标槽存在有效镜像头 */
    uint32_t running_version;    /**< 运行槽 image_version */
} app_boot_status_t;

/**
 * @brief 读取启动控制/槽状态（只读 flash，不修改）
 * @return 0 = 成功；-1 = flash 不可用
 */
int app_boot_get_status(app_boot_status_t *out);

/**
 * @brief 启动确认：pending == 本槽时写 active=本槽、pending=NONE
 * @return 0 = 已确认或无需确认；-1 = flash 写失败
 * @note 由 app_diag_step 在启动约 2s 后调用一次；非槽构建直接返回 0
 */
int app_boot_confirm(void);

/**
 * @brief 强制指定下次启动的 active 槽（调试/台架用；不改镜像内容）
 * @param slot 0=ACTIVE, 1=BACKUP
 * @return 0 = 成功；-1 = 非槽构建/非法槽/flash 写失败
 * @note 写入 active=slot、pending=NONE、attempts=0、confirmed=0；需重启生效
 */
int app_boot_set_active(uint8_t slot);

/**
 * @brief 进入 DFU：写启动邮箱并软复位（不返回）
 * @note 调用方必须先确认 FOC OFF / 桥已断开；本函数不再做联锁
 */
void app_boot_request_dfu(void) __attribute__((noreturn));

#endif /* APP_BOOT_H */
