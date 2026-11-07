/**
 * @file    boot.c
 * @brief   Bootloader 启动决策、槽校验与跳转
 * @author  Kaiser
 *
 * 决策顺序（与 docs/superpowers/specs/2026-11-07-ota-dfu-design.md 一致）：
 *   1. 邮箱请求 DFU → 直接进 DFU；
 *   2. pending 存在 → attempts+1 后尝试；超过 3 次或校验失败 → 清 pending 回滚；
 *   3. active 校验通过 → 启动；
 *   4. active 失败 → 尝试另一槽（成功则改写 active）；
 *   5. 两槽都无效 → 停在 DFU。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "boot.h"

#include "flash.h"

#include "hpm_csr_drv.h"
#include "hpm_interrupt.h"
#include "hpm_l1c_drv.h"
#include "hpm_soc.h"
#include "tusb.h"

#include <stddef.h>

static const boot_flash_ops_t *s_ops;

void boot_init(void) {
    boot_flash_init();
    s_ops = boot_flash_ops();
}

static const boot_flash_ops_t *ops(void) {
    return (s_ops != NULL) ? s_ops : boot_flash_ops();
}

/* ============================================================================
 * 槽校验
 * ============================================================================ */
bool boot_slot_valid(uint8_t slot_id) {
    const boot_image_header_t *hdr;
    uint32_t base;
    uint32_t crc;

    if (slot_id >= BOOT_SLOT_COUNT) {
        return false;
    }

    /* 只认“头合法 + payload CRC 一致”。镜像日志里的 FLASHING 不作为判据：
     * 半写镜像的头 CRC 必然对不上（新头的 size/crc 对应新 payload），
     * 而把“最后一条记录是 FLASHING”当失效会误伤 J-Link 直烧后的槽。 */
    base = boot_slot_address(slot_id);
    hdr = (const boot_image_header_t *) base;
    if (!boot_image_header_valid(hdr, slot_id)) {
        return false;
    }
    crc = boot_crc32((const void *) (base + BOOT_IMAGE_HEADER_SIZE), hdr->image_size);
    return crc == hdr->image_crc32;
}

/* ============================================================================
 * 启动控制
 * ============================================================================ */
static void control_load_or_default(boot_control_record_t *ctl) {
    if (boot_control_load(ops(), ctl) != 0) {
        boot_control_defaults(ctl);
    }
}

int boot_clear_pending(void) {
    boot_control_record_t ctl;

    control_load_or_default(&ctl);
    ctl.pending = BOOT_SLOT_NONE;
    ctl.attempts = 0U;
    return boot_control_save(ops(), &ctl);
}

int boot_slot_mark_flashing(uint8_t slot_id, const boot_image_header_t *hdr) {
    if ((hdr == NULL) || (slot_id >= BOOT_SLOT_COUNT)) {
        return -1;
    }
    return boot_image_record_begin(ops(), boot_slot_meta_address(slot_id), hdr->image_size,
                                   hdr->image_version);
}

int boot_slot_commit(uint8_t slot_id, const boot_image_header_t *hdr, uint32_t image_crc32) {
    boot_control_record_t ctl;
    int rc;

    if ((hdr == NULL) || (slot_id >= BOOT_SLOT_COUNT)) {
        return -1;
    }

    rc = boot_image_record_ready(ops(), boot_slot_meta_address(slot_id), hdr->image_size,
                                 image_crc32, hdr->image_version);
    if (rc != 0) {
        return rc;
    }

    control_load_or_default(&ctl);
    ctl.pending = slot_id;
    ctl.attempts = 0U;
    ctl.confirmed = 0U;
    return boot_control_save(ops(), &ctl);
}

/* ============================================================================
 * 启动决策
 * ============================================================================ */
boot_decision_t boot_decide(uint8_t *slot_out, bool force_dfu) {
    boot_control_record_t ctl;

    if (force_dfu) {
        return BOOT_DECISION_DFU;
    }

    control_load_or_default(&ctl);

    if (ctl.pending != BOOT_SLOT_NONE) {
        if (!boot_slot_valid(ctl.pending)) {
            (void) boot_clear_pending();
        } else if ((uint32_t) ctl.attempts + 1U > BOOT_MAX_ATTEMPTS) {
            /* 连续多次未能通过应用确认：回滚到已知可用的 active 槽 */
            (void) boot_clear_pending();
        } else {
            ctl.attempts++;
            (void) boot_control_save(ops(), &ctl);
            *slot_out = ctl.pending;
            return BOOT_DECISION_APP;
        }
    }

    if (boot_slot_valid(ctl.active)) {
        *slot_out = ctl.active;
        return BOOT_DECISION_APP;
    }

    {
        uint8_t other = (uint8_t) (ctl.active ^ 1U);

        if (boot_slot_valid(other)) {
            ctl.active = other;
            ctl.pending = BOOT_SLOT_NONE;
            ctl.attempts = 0U;
            ctl.confirmed = 0U;
            (void) boot_control_save(ops(), &ctl);
            *slot_out = other;
            return BOOT_DECISION_APP;
        }
    }

    return BOOT_DECISION_DFU;
}

/* ============================================================================
 * 跳转
 * ============================================================================ */
void boot_jump(uint8_t slot_id) {
    uint32_t base = boot_slot_address(slot_id);
    const boot_image_header_t *hdr = (const boot_image_header_t *) base;
    uint32_t image_bytes = BOOT_IMAGE_HEADER_SIZE + hdr->image_size;
    void (*entry)(void);

    /* 若本次运行经历过 DFU，先停掉 TinyUSB，避免应用重新初始化 USB 后
     * bootloader 的 ISR 仍然挂在同一控制器上。 */
    if (tusb_inited()) {
        (void) tud_deinit(0);
    }

    /* 同一上电周期内可能已执行过另一槽的旧镜像：清掉目标槽范围内的 I-cache 行，
     * 否则 DFU 重写后仍会取到旧指令。 */
    l1c_ic_invalidate(base, HPM_L1C_CACHELINE_ALIGN_UP(image_bytes));
    l1c_fence_i();

    entry = (void (*)(void)) (base + hdr->entry_offset);

    /* 关全局中断（应用的 start.S 会重置 mstatus 并重新使能） */
    __asm__ volatile("csrci mstatus, 8" ::: "memory");
    entry();

    for (;;) {
        /* 不应返回 */
    }
}
