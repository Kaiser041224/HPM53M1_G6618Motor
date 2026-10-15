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

#include "app_foc.h"
#include "app_motion.h"
#include "app_motor_params.h"
#include "app_software_params.h"
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
    csh_printf(
        csh, "foc: state=%s  theta=%.1f deg  omega=%.1f rad/s\r\n",
        app_terminal_cmd_foc_state_name(app_foc_get_state()),
        (double)(snap.theta_e_rad * APP_TERMINAL_FOC_DEG_PER_RAD), (double)snap.omega_e_rad_s);
    csh_printf(
        csh, "     i_d=%.3f/%.3f/%.3f A  i_q=%.3f/%.3f/%.3f A  (ref/avg/now)\r\n",
        (double)snap.i_d_ref_a, (double)snap.i_d_avg_a, (double)snap.i_d_a, (double)snap.i_q_ref_a,
        (double)snap.i_q_avg_a, (double)snap.i_q_a);
    csh_printf(
        csh, "     v_d=%.3f V  v_q=%.3f V  vbus=%.2f V  sat=%u scale=%.3f\r\n", (double)snap.v_d_v,
        (double)snap.v_q_v, (double)snap.v_bus_v, (unsigned)snap.saturated, (double)snap.v_scale);
    csh_printf(
        csh, "     voltage: duty_cap=%.3f vmax=%.3fV use=%.1f%% sat_count=%u/%u (cumulative)\r\n",
        (double)app_foc_current_duty_max(), (double)snap.voltage_max_v,
        (double)(snap.voltage_utilization * 100), (unsigned)snap.saturated_count,
        (unsigned)snap.run_count);
    csh_printf(
        csh, "     duty=%.3f/%.3f/%.3f  valid=%u  ok=%u  protect=%u  trip=%u  dt=%u us\r\n",
        (double)snap.duty_u, (double)snap.duty_v, (double)snap.duty_w, (unsigned)snap.valid,
        (unsigned)snap.run_count, (unsigned)snap.fault_count, (unsigned)snap.tripped,
        (unsigned)g_foc_loop_dt_us);
    /* M1 保护动作全关：判断计数（只增不动作），复测定位触发源用 */
    csh_printf(
        csh, "     judge: ovr=%u penc=%u pread=%u pvb=%u ptrip=%u\r\n",
        (unsigned)g_foc_isr_overruns, (unsigned)g_foc_protect_counts[APP_FOC_PROT_ENC],
        (unsigned)g_foc_protect_counts[APP_FOC_PROT_READ],
        (unsigned)g_foc_protect_counts[APP_FOC_PROT_VBUS],
        (unsigned)g_foc_protect_counts[APP_FOC_PROT_TRIP]);
    {
        float enc_offset;
        float enc_dir;

        app_foc_get_encoder_offset(&enc_offset, &enc_dir);
        csh_printf(
            csh, "     enc: offset=%.4f rad (%.1f deg) dir=%+.0f  (cal encoder show)\r\n",
            (double)enc_offset, (double)(enc_offset * APP_TERMINAL_FOC_DEG_PER_RAD),
            (double)enc_dir);
    }
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

        app_terminal_cmd_emit(
            "%u,%.3f,%.3f,%.3f,%.3f,%.2f\r\n", (unsigned)s_trace_sent, (double)s->i_d_a,
            (double)s->i_q_a, (double)s->v_d_v, (double)s->v_q_v, (double)s->theta_e_rad);
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
    if (strcmp(sub, "dutycap") == 0) {
        float ceiling;
        if (argc < 3) {
            csh_printf(
                csh, "dutycap effective=%.3f; use foc dutycap <0.70..0.98> while OFF\r\n",
                (double)app_foc_current_duty_max());
            return 0;
        }
        if (app_terminal_cmd_parse_float(argv[2], &ceiling) || app_foc_set_duty_ceiling(ceiling)) {
            csh_printf(csh, "ERR: require OFF and dutycap in [0.70,0.98]\r\n");
            return -1;
        }
        csh_printf(
            csh, "OK: RAM dutycap=%.3f (reboot restores 0.95); ADC window must be verified\r\n",
            (double)ceiling);
        return 0;
    }

    if (strcmp(sub, "on") == 0) {
        if (!app_terminal_cmd_require_no_fault(csh)) {
            return -1;
        }
        if (!app_terminal_cmd_require_motor_stopped(csh)) {
            return -1;
        }
        {
            int rc = app_foc_enable();

            if (rc != 0) {
                if (app_foc_get_state() == APP_FOC_STATE_FAULT) {
                    csh_printf(csh, "ERR: FOC latched in FAULT, run 'foc off' first\r\n");
                } else {
                    static const char* const why[] = {"?",
                                                      "?",
                                                      "fault state",
                                                      "adc invalid",
                                                      "encoder invalid",
                                                      "params invalid"};

                    csh_printf(
                        csh, "ERR: foc enable rejected (%s)\r\n",
                        ((rc <= -2) && (rc >= -5)) ? why[-rc] : "unknown");
                }
                return -1;
            }
        }
        app_terminal_cmd_foc_status(csh);
        return 0;
    }

    if (strcmp(sub, "off") == 0) {
        if (app_foc_request_stop()) {
            csh_printf(csh, "ERR: calibration/vtest owns output; cancel it before normal stop\r\n");
            return -1;
        }
        if (app_foc_stop_pending())
            csh_printf(csh, "foc: stopping asynchronously (~120ms); wait for state=OFF\r\n");
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
        csh_printf(
            csh,
            "vtest: v=%.3f V theta=%.1f deg 1.5s (expect |i|~%.2f A; check sign/mapping; "
            "'foc vtest off' stops)\r\n",
            (double)volts, (double)deg,
            (double)(volts / ((motor->rs_ohm > 0.0f) ? motor->rs_ohm : 1.0f)));
        return 0;
    }

    if (strcmp(sub, "bench") == 0) {
        /* 台架模式：一键关闭“自动停机”类保护（限流电源调试用；参数均为 LIVE）。
         * 仅关停保护，不改变控制律；vsat 降转矩（非停机）保持启用。 */
        app_software_params_t* software;
        uint8_t on;

        if (argc < 3) {
            csh_printf(csh, "usage: foc bench <0|1>  (1=关闭自动停机保护，0=恢复)\r\n");
            return -1;
        }
        on = (uint8_t)((argv[2][0] == '1') ? 1U : 0U);
        software = app_software_params_mutable();
        {
            /* 恢复值取编译期默认（config 下 yaml），避免硬编码漂移 */
            const app_software_params_t* defaults = app_software_params_default();

            software->control.limits.i_trip_a = on ? 0.0f : defaults->control.limits.i_trip_a;
            software->control.limits.speed_max_rad_s =
                on ? 0.0f : defaults->control.limits.speed_max_rad_s;
            software->fault.shutdown_en = on ? 0U : defaults->fault.shutdown_en;
        }
        csh_printf(
            csh, "bench=%u: i_trip_a=%.1f A  speed_max=%.0f rad/s  fault.shutdown_en=%u\r\n",
            (unsigned)on, (double)software->control.limits.i_trip_a,
            (double)software->control.limits.speed_max_rad_s,
            (unsigned)software->fault.shutdown_en);
        if (on != 0U) {
            csh_printf(
                csh, "WARN: auto-stop protections OFF (incl. bus OV/UV) "
                     "(use current-limited supply)\r\n");
        }
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
        csh_printf(
            csh, "trace: capturing %u samples @25kHz (~5ms)...\r\n", (unsigned)APP_FOC_TRACE_MAX);
        return 0;
    }

    return app_terminal_cmd_usage(
        csh, "foc [status] | foc on | foc off | foc vtest <v> [<deg>] | foc trace");
}

CSH_CMD_EXPORT_ALIAS(cmd_foc, foc, );

/** @brief motion拒绝码→处置提示 */
static const char* app_terminal_motion_reason(int rc) {
    switch (rc) {
    case APP_MOTION_CMD_BAD_PARAM: return "non-finite value or Kt invalid";
    case APP_MOTION_CMD_NOT_CALIBRATED: return "not calibrated/calibrating or direction changed";
    case APP_MOTION_CMD_STALE_FEEDBACK: return "feedback stale; wait 1s and check 'motion show'";
    case APP_MOTION_CMD_TARGET_LIMIT:
        return "target over limit (torque<=50Kt Nm; speed has no command limit)";
    case APP_MOTION_CMD_GAIN_NOT_SET:
        return "speed gains not set; run 'motion gains kp ki' while OFF";
    case APP_MOTION_CMD_FOC_NOT_READY:
        return "foc not READY/RUN; run 'foc on' ('motion off' stops foc)";
    case APP_MOTION_CMD_BASE_CHANGED:
        return "Kt/direction changed while active; 'motion off' first";
    case APP_MOTION_CMD_MODE_LOCKED: return "torque/speed switch while active; 'motion off' first";
    case APP_MOTION_CMD_OWNER_DENIED: return "calibration/vtest owns output";
    default: return "unknown";
    }
}
/** @brief motion模型转矩/轴端速度与静止安装标定 */
static int cmd_motion(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);
    const char* sub = argc > 1 ? argv[1] : "show";
    int rc = -1;
    float value, ki;
    if (strcmp(sub, "show") == 0) {
        app_motion_snapshot_t state;
        app_motion_get_snapshot(&state);
        csh_printf(csh, "phase trial done=%u (apply required)\r\n", state.calibration_done);
        csh_printf(
            csh, "reject: sample=%lu duplicate=%lu vernier=%lu timing=%lu\r\n",
            (unsigned long)state.sample_failure_count, (unsigned long)state.duplicate_count,
            (unsigned long)state.vernier_failure_count, (unsigned long)state.timing_failure_count);
        csh_printf(
            csh,
            "motion: cal=%u zero=%u valid=%u active=%u calibrating=%u samples=%lu reason=%u\r\n"
            "shaft=%.6frad speed=%.6frad/s phase=%.8fturn torque=%.4fNm iq=%.4fA "
            "good/bad=%lu/%lu\r\n"
            "exec: iq_acc=%.4fA hold=%lu lim=%u sat=%u\r\n",
            state.calibrated, state.zero_valid, state.feedback_valid, state.active,
            state.calibrating, (unsigned long)state.calibration_samples, (unsigned)state.reason,
            (double)state.shaft_rad, (double)state.speed_rad_s, (double)state.phase_turns,
            (double)state.torque_nm, (double)state.iq_a, (unsigned long)state.good_count,
            (unsigned long)state.bad_count, (double)state.iq_accepted_a,
            (unsigned long)state.hold_count, (unsigned)state.current_limited,
            (unsigned)state.voltage_saturated);
        return 0;
    }
    if (strcmp(sub, "cal") == 0)
        rc = app_motion_calibrate();
    else if (strcmp(sub, "apply") == 0)
        rc = app_motion_apply();
    else if (strcmp(sub, "zero") == 0)
        rc = app_motion_zero();
    else if (strcmp(sub, "save") == 0)
        rc = app_motion_save();
    else if (strcmp(sub, "off") == 0) {
        rc = app_motion_stop();
    } else if (strcmp(sub, "direct") == 0)
        rc = app_foc_set_current_source(APP_FOC_CURRENT_SOURCE_DIRECT);
    else if (argc >= 3 && app_terminal_cmd_parse_float(argv[2], &value) == 0) {
        if (strcmp(sub, "torque") == 0)
            rc = app_motion_torque(value);
        else if (strcmp(sub, "speed") == 0)
            rc = app_motion_speed(value);
        else if (
            strcmp(sub, "gains") == 0 && argc >= 4
            && app_terminal_cmd_parse_float(argv[3], &ki) == 0) {
            float friction_feedforward_nm = 0;
            if (argc >= 5 && app_terminal_cmd_parse_float(argv[4], &friction_feedforward_nm) != 0)
                return app_terminal_cmd_usage(
                    csh, "motion gains kp ki [friction_feedforward_nm] (OFF)");
            rc = app_motion_gains(value, ki, friction_feedforward_nm);
        } else
            return app_terminal_cmd_usage(
                csh, "motion gains kp ki [friction_feedforward_nm] (OFF)");
    } else
        return app_terminal_cmd_usage(
            csh, "motion show|cal (OFF stationary)|apply|zero|save|gains kp ki "
                 "[friction_feedforward_nm] (OFF)|"
                 "torque Nm|speed rad/s|off|direct (OFF)");
    csh_printf(
        csh,
        "%s: motion rc=%d%s%s%s; show|cal (OFF stationary)|apply|zero|save|"
        "gains kp ki [friction_feedforward_nm] (OFF)|torque Nm|speed rad/s|off|direct (OFF)\r\n",
        rc == 0 ? "accepted" : "ERR", rc, rc == 0 ? "" : " (",
        rc == 0 ? "" : app_terminal_motion_reason(rc), rc == 0 ? "" : ")");
    return rc;
}
CSH_CMD_EXPORT_ALIAS(cmd_motion, motion, );
