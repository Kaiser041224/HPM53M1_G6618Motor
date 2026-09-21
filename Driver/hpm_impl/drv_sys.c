/**
 * @file    drv_sys.c
 * @brief   System 驱动实现（复位状态查询 + 全局中断临界区）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_sys.h"

#include "hpm_common.h"
#include "hpm_interrupt.h"
#include "hpm_soc.h"
#include "hpm_ppor_drv.h"

uint32_t intf_sys_get_reset_status(void)
{
    return ppor_reset_get_status(HPM_PPOR);
}

uint32_t intf_sys_irq_save(void)
{
    return disable_global_irq(CSR_MSTATUS_MIE_MASK);
}

void intf_sys_irq_restore(uint32_t state)
{
    restore_global_irq(state);
}
