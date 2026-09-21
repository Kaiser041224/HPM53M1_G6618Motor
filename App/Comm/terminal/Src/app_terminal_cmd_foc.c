/**
 * @file    app_terminal_cmd_foc.c
 * @brief   Terminal FOC 命令实现（foc [status] | foc on | foc off）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_terminal_cmd_foc.h"

#include "app_terminal_cmd.h"

#include <string.h>

#define APP_TERMINAL_FOC_DEG_PER_RAD (57.2957795f)

const char* app_terminal_cmd_foc_state_name(app_foc_state_t state) {
    switch (state) {
    case APP_FOC_STATE_OFF: return "OFF";
    case APP_FOC_STATE_READY: return "READY";
    case APP_FOC_STATE_RUN: return "RUN";
    case APP_FOC_STATE_CALIB: return "CALIB";
    case APP_FOC_STATE_FAULT: return "FAULT";
    default: return "?";
    }
}

void app_terminal_cmd_foc_status(chry_shell_t* csh) {
    app_foc_current_snapshot_t snap;

    app_foc_get_snapshot(&snap);
    csh_printf(csh, "foc: state=%s  theta=%.1f deg  omega=%.1f rad/s\r\n",
               app_terminal_cmd_foc_state_name(app_foc_get_state()),
               (double)(snap.theta_e_rad * APP_TERMINAL_FOC_DEG_PER_RAD),
               (double)snap.omega_e_rad_s);
    csh_printf(csh, "     i_d=%.3f/%.3f A  i_q=%.3f/%.3f A  (ref/meas)\r\n",
               (double)snap.i_d_ref_a, (double)snap.i_d_a, (double)snap.i_q_ref_a,
               (double)snap.i_q_a);
    csh_printf(csh, "     v_d=%.3f V  v_q=%.3f V  vbus=%.2f V  sat=%u scale=%.3f\r\n",
               (double)snap.v_d_v, (double)snap.v_q_v, (double)snap.v_bus_v,
               (unsigned)snap.saturated, (double)snap.v_scale);
    csh_printf(csh, "     duty=%.3f/%.3f/%.3f  valid=%u  ok=%u  fault=%u\r\n",
               (double)snap.duty_u, (double)snap.duty_v, (double)snap.duty_w,
               (unsigned)snap.valid, (unsigned)snap.run_count, (unsigned)snap.fault_count);
}

/**
 * @brief 命令 foc：status / on / off
 */
static int cmd_foc(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);
    const char* sub = (argc >= 2) ? argv[1] : "status";

    if (strcmp(sub, "status") == 0) {
        app_terminal_cmd_foc_status(csh);
        return 0;
    }

    if (strcmp(sub, "on") == 0) {
        if (!app_terminal_cmd_require_no_fault(csh)) {
            return -1;
        }
        if (!app_terminal_cmd_require_motor_stopped(csh)) {
            return -1;
        }
        if (app_foc_enable() != 0) {
            csh_printf(csh, "ERR: foc enable rejected (fault/adc/encoder/params)\r\n");
            return -1;
        }
        app_terminal_cmd_foc_status(csh);
        return 0;
    }

    if (strcmp(sub, "off") == 0) {
        app_foc_disable();
        app_terminal_cmd_foc_status(csh);
        return 0;
    }

    return app_terminal_cmd_usage(csh, "foc [status] | foc on | foc off");
}

CSH_CMD_EXPORT_ALIAS(cmd_foc, foc, );
