/**
 * @file    main.c
 * @brief   G6618 DFU Bootloader 入口：时钟 → 启动决策 → 跳应用 / 进 DFU
 * @author  Kaiser
 *
 * 启动路径（详见 Interface/intf_boot.h 与 OTA 设计文档）：
 *   board_init → intf_clock_init → boot_init
 *   → 邮箱 DFU 请求？ → dfu_run(非活动槽)
 *   → boot_decide：pending 优先（≤3 次尝试）、active 兜底、两槽都坏进 DFU
 *   → boot_jump(slot)
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "boot.h"
#include "dfu.h"
#include "flash.h"

#include "board.h"
#include "intf_clock.h"

int main(void) {
    boot_control_record_t control;
    bool active_valid;
    uint8_t target_slot;
    uint8_t slot = 0U;
    bool dfu_requested;

    board_init();
    intf_clock_init();
    boot_init();

    dfu_requested = boot_mailbox_consume_dfu();

    if (!dfu_requested && (boot_decide(&slot, false) == BOOT_DECISION_APP)) {
        boot_jump(slot);
    }

    if (boot_control_load(boot_flash_ops(), &control) != 0) {
        boot_control_defaults(&control);
    }
    active_valid = boot_slot_valid(control.active);

    /* 目标槽：active 可用 → 写另一槽（保留可回退镜像）；
     * active 本身无效（例如只烧了 bootloader）→ 直接写 active，首次安装落到 A。
     * 这样空片首次 DFU 应发送 slot_active.bin，而不是 slot_backup.bin。 */
    target_slot = active_valid ? (uint8_t) (control.active ^ 1U) : control.active;

    dfu_run(target_slot, active_valid);

    return 0;
}
