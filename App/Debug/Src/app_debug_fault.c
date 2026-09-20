/*
 * Debug Fault - 故障保护调试实现
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_debug_fault.h"

#include "app_debug_rtt.h"
#include "app_fault.h"

#include <stddef.h>

static const char *const s_state_names[] = {
    "INIT", "NORMAL", "WARNING", "FAULT",
};

void app_debug_fault_dump(void) {
    uint16_t cnt[10] = {0};
    app_fault_snapshot_t snap;
    uint32_t state = (uint32_t) app_fault_get_state();

    app_debug_printf("[FLT] state=%s codes=0x%08X latched=0x%08X first=0x%08X\r\n",
                     (state < 4U) ? s_state_names[state] : "?",
                     (unsigned) app_fault_get_codes(), (unsigned) app_fault_get_latched(),
                     (unsigned) app_fault_get_first());

    app_fault_get_event_counts(cnt, 10U);
    app_debug_printf(
        "[FLT] cnt: F_U=%u F_V=%u F_W=%u S_U=%u S_V=%u S_W=%u OV=%u UV=%u TO=%u ENC=%u\r\n",
        (unsigned) cnt[0], (unsigned) cnt[1], (unsigned) cnt[2], (unsigned) cnt[3],
        (unsigned) cnt[4], (unsigned) cnt[5], (unsigned) cnt[6], (unsigned) cnt[7],
        (unsigned) cnt[8], (unsigned) cnt[9]);

    if (app_fault_get_snapshot(&snap)) {
        app_debug_printf(
            "[FLT] trip: code=0x%08X iu=%.3fA iv=%.3fA iw=%.3fA vbus=%.2fV raw=%u\r\n",
            (unsigned) snap.code, (double) snap.i_u_a, (double) snap.i_v_a,
            (double) snap.i_w_a, (double) snap.v_bus_v, (unsigned) snap.oc_fast_raw);
    } else {
        app_debug_printf("[FLT] trip: (none)\r\n");
    }
}

void app_debug_fault_clear(void) {
    if (app_fault_clear() == 0) {
        app_debug_printf("[FLT] clear OK\r\n");
    } else {
        app_debug_printf("[FLT] clear REJECTED (condition active)\r\n");
    }
}
