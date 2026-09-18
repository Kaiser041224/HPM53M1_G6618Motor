/*
 * System Driver Implementation (HPM SDK)
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_sys.h"

#include "hpm_common.h"
#include "hpm_soc.h"
#include "hpm_ppor_drv.h"

uint32_t intf_sys_get_reset_status(void)
{
    return ppor_reset_get_status(HPM_PPOR);
}
