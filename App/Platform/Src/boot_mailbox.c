/**
 * @file    boot_mailbox.c
 * @brief   启动邮箱（SYSCTL CPU0 GPR）与软件复位（bootloader / 应用共用）
 * @author  Kaiser
 *
 * 软复位后 SYSCTL GPR 保持，bootloader 据此判断"本次复位是应用请求进 DFU"。
 * 邮箱写入后必须立刻复位，中间不要插入其它可能被复位打断的状态。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_boot.h"

#include "hpm_common.h"
#include "hpm_soc.h"
#include "hpm_ppor_drv.h"
#include "hpm_sysctl_drv.h"

void boot_mailbox_request_dfu(void) {
    uint32_t values[2] = {BOOT_MAILBOX_MAGIC, BOOT_MAILBOX_REQUEST_DFU};

    (void) sysctl_cpu0_set_gpr(HPM_SYSCTL, BOOT_MAILBOX_GPR_INDEX, 2U, values, false);
}

bool boot_mailbox_consume_dfu(void) {
    uint32_t values[2] = {0U, 0U};
    bool requested;

    if (sysctl_cpu0_get_gpr(HPM_SYSCTL, BOOT_MAILBOX_GPR_INDEX, 2U, values) != status_success) {
        return false;
    }
    requested = (values[0] == BOOT_MAILBOX_MAGIC) && (values[1] == BOOT_MAILBOX_REQUEST_DFU);
    if (requested) {
        values[0] = 0U;
        values[1] = 0U;
        (void) sysctl_cpu0_set_gpr(HPM_SYSCTL, BOOT_MAILBOX_GPR_INDEX, 2U, values, false);
    }
    return requested;
}

void boot_system_reset(void) {
    ppor_reset_mask_set_source_enable(HPM_PPOR, ppor_reset_software);
    ppor_reset_set_hot_reset_enable(HPM_PPOR, ppor_reset_software);
    ppor_sw_reset(HPM_PPOR, 10U);
    for (;;) {
    }
}
