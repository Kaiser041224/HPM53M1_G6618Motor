/**
 * @file    app_boot.c
 * @brief   应用侧 OTA/DFU 支持实现（启动确认 / 槽信息 / 进入 DFU）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_boot.h"

#include "intf_boot.h"
#include "intf_flash.h"

#include <stddef.h>

/* ============================================================================
 * flash 后端适配（intf_flash → boot_flash_ops_t）
 * ============================================================================ */
static int boot_ops_read(uint32_t address, void *buf, size_t len) {
    const intf_flash_t *flash = intf_flash_get();

    return (flash != NULL) ? flash->read(address, buf, len) : -1;
}

static int boot_ops_erase(uint32_t address) {
    const intf_flash_t *flash = intf_flash_get();

    return (flash != NULL) ? flash->erase_sector(address) : -1;
}

static int boot_ops_program(uint32_t address, const void *buf, size_t len) {
    const intf_flash_t *flash = intf_flash_get();

    return (flash != NULL) ? flash->program(address, buf, len) : -1;
}

static const boot_flash_ops_t *boot_ops(void) {
    static const boot_flash_ops_t ops = {
        .read = boot_ops_read,
        .erase_sector = boot_ops_erase,
        .program = boot_ops_program,
    };

    return (intf_flash_get() != NULL) ? &ops : NULL;
}

/* ============================================================================
 * 身份
 * ============================================================================ */
bool app_boot_is_slot_build(void) {
#ifdef APP_SLOT_ID
    return true;
#else
    return false;
#endif
}

uint8_t app_boot_slot_id(void) {
#ifdef APP_SLOT_ID
    return (uint8_t) APP_SLOT_ID;
#else
    return BOOT_SLOT_NONE;
#endif
}

uint32_t app_boot_image_version(void) {
#ifdef APP_SLOT_ID
    const boot_image_header_t *hdr =
        (const boot_image_header_t *) boot_slot_address((uint8_t) APP_SLOT_ID);

    return boot_image_header_valid(hdr, (uint8_t) APP_SLOT_ID) ? hdr->image_version : 0U;
#else
    return 0U;
#endif
}

uint32_t app_boot_image_crc(void) {
#ifdef APP_SLOT_ID
    const boot_image_header_t *hdr =
        (const boot_image_header_t *) boot_slot_address((uint8_t) APP_SLOT_ID);

    return boot_image_header_valid(hdr, (uint8_t) APP_SLOT_ID) ? hdr->image_crc32 : 0U;
#else
    return 0U;
#endif
}

int app_boot_self_verify(void) {
#ifdef APP_SLOT_ID
    const uint8_t slot = (uint8_t) APP_SLOT_ID;
    const boot_image_header_t *hdr = (const boot_image_header_t *) boot_slot_address(slot);
    uint32_t crc;

    if (!boot_image_header_valid(hdr, slot)) {
        return -1;
    }
    crc = boot_crc32((const void *) (boot_slot_address(slot) + BOOT_IMAGE_HEADER_SIZE),
                     hdr->image_size);
    return (crc == hdr->image_crc32) ? 0 : -2;
#else
    return -1;
#endif
}

/* ============================================================================
 * 状态 / 确认
 * ============================================================================ */
int app_boot_get_status(app_boot_status_t *out) {
    const boot_flash_ops_t *ops;
    boot_control_record_t control;

    if (out == NULL) {
        return -1;
    }

    out->control_valid = false;
    out->active_slot = 0U;
    out->pending_slot = BOOT_SLOT_NONE;
    out->attempts = 0U;
    out->confirmed = 0U;
    out->running_slot = app_boot_slot_id();
    out->upgrade_target_slot = 1U; /* 默认启动 ACTIVE → 升级目标 BACKUP */
    out->upgrade_target_valid = false;
    out->running_version = app_boot_image_version();

    ops = boot_ops();
    if (ops == NULL) {
        return -1;
    }

    if (boot_control_load(ops, &control) == 0) {
        out->control_valid = true;
        out->active_slot = control.active;
        out->pending_slot = control.pending;
        out->attempts = control.attempts;
        out->confirmed = control.confirmed;
    }
    /* 进入 DFU 前应用会先确认运行槽（active=running），因此"下一次 DFU 目标"
     * 以运行槽为准：running ^ 1。仅在未拿到运行槽号时退回 active ^ 1。 */
    out->upgrade_target_slot = (out->running_slot < BOOT_SLOT_COUNT)
                                   ? (uint8_t) (out->running_slot ^ 1U)
                                   : (uint8_t) (out->active_slot ^ 1U);
    out->upgrade_target_valid = boot_image_header_valid(
        (const boot_image_header_t *) boot_slot_address(out->upgrade_target_slot),
        out->upgrade_target_slot);
    return 0;
}

int app_boot_confirm(void) {
#ifdef APP_SLOT_ID
    const boot_flash_ops_t *ops = boot_ops();

    if (ops == NULL) {
        return -1;
    }
    return boot_confirm_running_slot(ops, (uint8_t) APP_SLOT_ID);
#else
    return 0;
#endif
}

int app_boot_set_active(uint8_t slot) {
#ifdef APP_SLOT_ID
    const boot_flash_ops_t *ops = boot_ops();
    boot_control_record_t rec;

    if ((ops == NULL) || (slot >= BOOT_SLOT_COUNT)) {
        return -1;
    }
    if (boot_control_load(ops, &rec) != 0) {
        boot_control_defaults(&rec);
    }
    rec.active = slot;
    rec.pending = BOOT_SLOT_NONE;
    rec.attempts = 0U;
    rec.confirmed = 0U;
    return boot_control_save(ops, &rec);
#else
    (void) slot;
    return -1;
#endif
}

/* ============================================================================
 * 进入 DFU
 * ============================================================================ */
void app_boot_request_dfu(void) {
    /* 先把当前运行槽确认为 active：否则在“pending 槽已启动但尚未确认”的窗口内
     * 进 DFU，bootloader 的 target = active^1 会等于正在运行的槽。
     * 确认失败也继续（active 仍是可回退镜像，最坏是重写运行槽）。 */
    (void) app_boot_confirm();

    /* 必须先写邮箱再复位；复位类型用 hot reset（保留 SYSCTL GPR），
     * 否则冷复位会清掉邮箱，bootloader 将看不到 DFU 请求。 */
    boot_mailbox_request_dfu();
    boot_system_reset();

    for (;;) {
        /* 不应返回 */
    }
}
