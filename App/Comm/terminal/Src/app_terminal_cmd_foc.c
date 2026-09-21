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

#include "app_motor_params.h"
#include "app_terminal.h"
#include "app_terminal_job.h"

#include <string.h>

#define APP_TERMINAL_FOC_DEG_PER_RAD (57.2957795f)
#define APP_TERMINAL_FOC_RAD_PER_DEG (0.01745329252f)

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
    csh_printf(csh, "     i_d=%.3f/%.3f/%.3f A  i_q=%.3f/%.3f/%.3f A  (ref/avg/now)\r\n",
               (double)snap.i_d_ref_a, (double)snap.i_d_avg_a, (double)snap.i_d_a,
               (double)snap.i_q_ref_a, (double)snap.i_q_avg_a, (double)snap.i_q_a);
    csh_printf(csh, "     v_d=%.3f V  v_q=%.3f V  vbus=%.2f V  sat=%u scale=%.3f\r\n",
               (double)snap.v_d_v, (double)snap.v_q_v, (double)snap.v_bus_v,
               (unsigned)snap.saturated, (double)snap.v_scale);
    csh_printf(csh, "     duty=%.3f/%.3f/%.3f  valid=%u  ok=%u  protect=%u  trip=%u  dt=%u us\r\n",
               (double)snap.duty_u, (double)snap.duty_v, (double)snap.duty_w,
               (unsigned)snap.valid, (unsigned)snap.run_count, (unsigned)snap.fault_count,
               (unsigned)snap.tripped, (unsigned)g_foc_loop_dt_us);
    if (app_foc_get_state() == APP_FOC_STATE_FAULT) {
        csh_printf(csh, "     hint: run 'foc off' then 'foc on' to recover\r\n");
    }
}

/* ── foc trace：捕获 25kHz 波形并以 CSV 输出（每拍最多 8 行，避免占满 TX 环） ── */
static uint32_t s_trace_ticks;
static uint16_t s_trace_sent;

/**
 * @brief foc trace job：等待采满后逐行输出 CSV
 * @param now_ms 系统毫秒计数（未用）
 */
static void foc_trace_tick(uint32_t now_ms) {
    uint16_t n;
    const app_foc_trace_sample_t* data = app_foc_trace_data(&n);

    (void)now_ms;
    s_trace_ticks++;
    if ((n < APP_FOC_TRACE_MAX) && (s_trace_ticks < 3000U)) {
        return; /* 等待采满（最长 3s） */
    }
    if (n == 0U) {
        app_terminal_cmd_emit("FAIL: no trace samples (FOC not running?)\r\n");
        app_terminal_job_abort();
        return;
    }
    for (uint8_t i = 0U; (i < 8U) && (s_trace_sent < n); i++) {
        const app_foc_trace_sample_t* s = &data[s_trace_sent];

        app_terminal_cmd_emit("%u,%.3f,%.3f,%.3f,%.3f,%.2f\r\n", (unsigned)s_trace_sent,
                              (double)s->i_d_a, (double)s->i_q_a, (double)s->v_d_v,
                              (double)s->v_q_v, (double)s->theta_e_rad);
        s_trace_sent++;
    }
    if (s_trace_sent >= n) {
        app_terminal_cmd_emit("trace: %u samples (csv: idx,id,iq,vd,vq,theta)\r\n", (unsigned)n);
        app_terminal_job_abort();
    }
}

/**
 * @brief foc trace job 中止回调
 */
static void foc_trace_abort(void) {
    app_terminal_cmd_emit("\r\n");
    app_terminal_refresh();
}

/** foc trace job（静态生命周期） */
static app_terminal_job_t s_foc_trace_job = {
    .name = "foc trace",
    .tick = foc_trace_tick,
    .abort = foc_trace_abort,
    .active = false,
};

/**
 * @brief 命令 foc：status / on / off / vtest / trace
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
            if (app_foc_get_state() == APP_FOC_STATE_FAULT) {
                csh_printf(csh, "ERR: FOC latched in FAULT, run 'foc off' first\r\n");
            } else {
                csh_printf(csh, "ERR: foc enable rejected (fault/adc/encoder/params)\r\n");
            }
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

    if (strcmp(sub, "vtest") == 0) {
        float volts;
        float deg = 0.0f;
        const app_motor_params_t* motor = app_motor_params_current();

        if ((argc >= 3) && (strcmp(argv[2], "off") == 0)) {
            app_foc_current_vtest_stop();
            csh_printf(csh, "vtest: stopped\r\n");
            return 0;
        }
        if (argc < 3) {
            return app_terminal_cmd_usage(csh, "foc vtest <volts> [<deg>] | foc vtest off");
        }
        if (app_terminal_cmd_parse_float(argv[2], &volts) != 0) {
            csh_printf(csh, "ERR: invalid volts '%s'\r\n", argv[2]);
            return -1;
        }
        if ((argc >= 4) && (app_terminal_cmd_parse_float(argv[3], &deg) != 0)) {
            csh_printf(csh, "ERR: invalid deg '%s'\r\n", argv[3]);
            return -1;
        }
        if ((app_foc_get_state() != APP_FOC_STATE_READY)
            && (app_foc_get_state() != APP_FOC_STATE_RUN)) {
            csh_printf(csh, "ERR: FOC not enabled (use 'foc on' first)\r\n");
            return -1;
        }
        if (app_foc_current_vtest_start(volts, deg * APP_TERMINAL_FOC_RAD_PER_DEG, 1.5f) != 0) {
            csh_printf(csh, "ERR: vtest start failed\r\n");
            return -1;
        }
        csh_printf(csh,
                   "vtest: v=%.3f V theta=%.1f deg 1.5s (expect |i|~%.2f A; check sign/mapping; "
                   "'foc vtest off' stops)\r\n",
                   (double)volts, (double)deg,
                   (double)(volts / ((motor->rs_ohm > 0.0f) ? motor->rs_ohm : 1.0f)));
        return 0;
    }

    if (strcmp(sub, "trace") == 0) {
        if ((app_foc_get_state() != APP_FOC_STATE_READY)
            && (app_foc_get_state() != APP_FOC_STATE_RUN)) {
            csh_printf(csh, "ERR: FOC not enabled (use 'foc on' first)\r\n");
            return -1;
        }
        app_foc_trace_reset();
        if (app_foc_trace_arm() != 0) {
            csh_printf(csh, "ERR: trace arm failed\r\n");
            return -1;
        }
        s_trace_ticks = 0U;
        s_trace_sent = 0U;
        if (app_terminal_job_start(&s_foc_trace_job) != 0) {
            csh_printf(csh, "FAIL: job start\r\n");
            return -1;
        }
        csh_printf(csh, "trace: capturing %u samples @25kHz (~5ms)...\r\n",
                   (unsigned)APP_FOC_TRACE_MAX);
        return 0;
    }

    return app_terminal_cmd_usage(
        csh, "foc [status] | foc on | foc off | foc vtest <v> [<deg>] | foc trace");
}

CSH_CMD_EXPORT_ALIAS(cmd_foc, foc, );
