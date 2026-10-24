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

#include "app_terminal.h"
#include "app_terminal_job.h"

#include "app_3phase_inverter.h"
#include "app_foc.h"
#include "app_friction_identify.h"
#include "app_motion.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "app_terminal.h"
#include "app_terminal_job.h"

#include <string.h>

#define APP_TERMINAL_FOC_DEG_PER_RAD (57.2957795f)
#define APP_TERMINAL_FOC_RAD_PER_DEG (0.01745329252f)
#define APP_TERMINAL_FOC_RAD_PER_RPM (0.10471975512f) /* 2π/60：rpm → 轴端 rad/s */

static const char k_motion_usage[] =
    "motion show|cal (OFF stationary)|apply|zero [save]|save|"
    "gains kp ki [coulomb] [viscous] (idle)|friction show|cal|apply|save|"
    "torque Nm|speed rpm|pos deg[0~360] [vel_ff] [torque_ff]|"
    "pos_gains p [bw] (idle)|off|direct (OFF)";

static int foc_complete_off(chry_shell_t* csh, const char* who);

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
        const app_motor_params_t* motor = app_motor_params_current();

        app_foc_get_encoder_offset(&enc_offset, &enc_dir);
        csh_printf(
            csh, "     enc: offset=%.4f rad (%.1f deg) dir=%+.0f valid=%u  (cal encoder show)\r\n",
            (double)enc_offset, (double)(enc_offset * APP_TERMINAL_FOC_DEG_PER_RAD),
            (double)enc_dir, (unsigned)motor->encoder.electrical_offset_valid);
        if (motor->encoder.electrical_offset_valid == 0U) {
            csh_printf(csh, "     WARN: encoder offset uncalibrated; commands continue\r\n");
        }
    }
    if (app_foc_get_state() == APP_FOC_STATE_FAULT) {
        csh_printf(csh, "     hint: run 'foc off' then 'foc on' to recover\r\n");
    }
}

/* ── foc trace：捕获 30kHz 波形并以 CSV 输出（每拍最多 8 行，避免占满 TX 环） ── */
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
        if (foc_complete_off(csh, "foc") != 0) {
            return -1;
        }
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
            csh, "bench=%u: i_trip_a=%.1f A  foc_speed_max=%.0f rad/s  fault.shutdown_en=%u\r\n",
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
            csh, "trace: capturing %u samples @30kHz (~4.3ms)...\r\n", (unsigned)APP_FOC_TRACE_MAX);
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
        return "gains not set; run 'motion gains kp ki' (speed) or 'motion pos_gains p [bw]' "
               "(position) while OFF";
    case APP_MOTION_CMD_FOC_NOT_READY:
        return "foc not READY/RUN; run 'foc on' ('motion off' stops foc)";
    case APP_MOTION_CMD_BASE_CHANGED:
        return "Kt/direction changed while active; 'motion off' first";
    case APP_MOTION_CMD_MODE_LOCKED:
        return "torque/speed/position switch while active; 'motion off' first";
    case APP_MOTION_CMD_OWNER_DENIED: return "calibration/vtest owns output";
    default: return "unknown";
    }
}

/** @brief motion 顶层用法 + 生命周期提示（zero/apply/save 语义容易混淆） */
static int motion_usage(chry_shell_t* csh) {
    int rc = app_terminal_cmd_usage(csh, k_motion_usage);

    csh_printf(
        csh,
        "  zero: RAM only | zero save: RAM+flash | apply: phase RAM (zero invalidated) | "
        "save: phase+zero flash\r\n"
        "  user direction: software.motion.output_direction = +1/-1 (reboot required)\r\n");
    return rc;
}

/**
 * @brief 阻塞等待 FOC 完整 OFF，然后全关栅极供电并打印完成提示
 * @return 0 = OFF + gate off；-1 = 超时/已强制关闭
 * @note 仅由 Terminal 命令上下文调用；等待期间命令层不再接受下一条指令。
 */
static int foc_complete_off(chry_shell_t* csh, const char* who) {
    bool ok = app_foc_wait_off(3000U);

    app_3phase_inverter_disable(); /* 完整 OFF 后才关 +12V */
    if (!ok) {
        csh_printf(csh, "ERR: %s stop timeout; outputs forced OFF, gate power OFF\r\n", who);
        return -1;
    }
    csh_printf(csh, "%s: state=OFF, gate power OFF\r\n", who);
    return 0;
}

/* ============================================================================
 * motion friction cal：摩擦辨识 job（200ms 进度条，照 cal encoder 形式）
 * ============================================================================ */

static uint32_t s_friction_beat_ms;

/** @brief 摩擦辨识失败原因文本 */
static const char* friction_failure_name(app_friction_identify_failure_t failure) {
    switch (failure) {
    case APP_FRICTION_IDENTIFY_FAIL_MOTION: return "motion aborted";
    case APP_FRICTION_IDENTIFY_FAIL_NOT_SETTLED: return "speed not settled";
    case APP_FRICTION_IDENTIFY_FAIL_FIT: return "fit rejected";
    case APP_FRICTION_IDENTIFY_FAIL_RESIDUAL: return "residual over gate";
    case APP_FRICTION_IDENTIFY_FAIL_ASYMMETRY: return "asymmetry over gate";
    default: return "none";
    }
}

/** @brief friction cal job：进度显示（200ms）与结束打印（结果/质量恒显示） */
static void friction_cal_tick(uint32_t now_ms) {
    app_friction_identify_snapshot_t identify;

    app_friction_identify_get_snapshot(&identify);
    if (app_friction_identify_busy()) {
        if ((uint32_t)(now_ms - s_friction_beat_ms) >= 200U) {
            uint32_t pct = (uint32_t)(identify.progress * 100.0f);
            char bar[21];

            s_friction_beat_ms = now_ms;
            if (pct > 100U)
                pct = 100U;
            for (uint32_t i = 0U; i < 20U; i++)
                bar[i] = (i < (pct / 5U)) ? '#' : '-';
            bar[20] = '\0';
            app_terminal_cmd_emit(
                "\rmotion friction [%s] %3u%%  %4.1fs  pt=%lu/12   ", bar, (unsigned)pct,
                (double)identify.elapsed_ms / 1000.0, (unsigned long)identify.point_index);
        }
        return;
    }
    app_terminal_cmd_emit(
        "\r\n%s: friction identify (%s) %4.1fs pt=%lu/12 retries=%lu\r\n",
        identify.result_valid ? "OK" : "FAIL", friction_failure_name(identify.failure),
        (double)identify.elapsed_ms / 1000.0, (unsigned long)identify.point_index,
        (unsigned long)identify.command_retries);
    app_terminal_cmd_emit(
        "    Tc=%.4fNm Kv=%.4fNm/(rad/s) residual=%.4fNm asym=%.4fNm\r\n",
        (double)identify.result.coulomb_nm, (double)identify.result.viscous_nm_per_rad_s,
        (double)identify.residual_rms_nm, (double)identify.asymmetry_nm);
    app_terminal_cmd_emit(
        "    motion: reason=%u good=%lu bad=%lu hold=%lu\r\n", (unsigned)identify.motion_reason,
        (unsigned long)identify.good_count, (unsigned long)identify.bad_count,
        (unsigned long)identify.hold_count);
    if (identify.result_valid)
        app_terminal_cmd_emit(
            "    next: 'motion friction apply' (RAM) -> 'motion friction save' (flash)\r\n");
    else
        app_terminal_cmd_emit(
            "    fit values kept for inspection (not applicable); retry 'motion friction cal "
            "[tol_pct]' to loosen gate\r\n");
    app_terminal_job_abort(); /* 完成/失败：结束 job（触发 abort 回调换行+刷新） */
}

/** @brief friction cal job 中止回调（用户取消或完成） */
static void friction_cal_abort(void) {
    app_friction_identify_abort();
    app_terminal_cmd_emit("\r\n");
    app_terminal_refresh();
}

/** friction cal job（静态生命周期） */
static app_terminal_job_t s_friction_cal_job = {
    .name = "motion friction cal",
    .tick = friction_cal_tick,
    .abort = friction_cal_abort,
    .active = false,
};

/** @brief motion模型转矩/轴端速度与静止安装标定 */
static int cmd_motion(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);
    const char* sub = argc > 1 ? argv[1] : "show";
    int rc = -1;
    float value, ki;
    if (strcmp(sub, "friction") == 0) {
        const char* action = argc > 2 ? argv[2] : "show";
        app_friction_identify_snapshot_t identify;
        app_friction_identify_get_snapshot(&identify);
        if (strcmp(action, "show") == 0) {
            csh_printf(
                csh,
                "friction: state=%u failure=%u point=%lu valid=%u\r\n"
                "result: coulomb=%.4fNm viscous=%.4fNm/(rad/s) smooth=%.3frad/s\r\n"
                "quality: residual=%.4fNm asymmetry=%.4fNm\r\n",
                (unsigned)identify.state, (unsigned)identify.failure,
                (unsigned long)identify.point_index, (unsigned)identify.result_valid,
                (double)identify.result.coulomb_nm, (double)identify.result.viscous_nm_per_rad_s,
                (double)identify.result.smoothing_rad_s, (double)identify.residual_rms_nm,
                (double)identify.asymmetry_nm);
            return 0;
        }
        if (app_friction_identify_busy()) {
            csh_printf(csh, "ERR: friction identify busy; 'motion off' aborts it\r\n");
            return -1;
        }
        if (strcmp(action, "cal") == 0) {
            float tolerance = 0;
            if (argc >= 4 && app_terminal_cmd_parse_float(argv[3], &tolerance) != 0)
                return app_terminal_cmd_usage(csh, "motion friction cal [tol_pct]");
            if (tolerance != 0)
                tolerance /= 100.0f; /* 百分比输入 */
            if (app_terminal_job_start(&s_friction_cal_job) != 0) {
                csh_printf(csh, "FAIL: job start\r\n");
                return -1;
            }
            s_friction_beat_ms = 0U;
            int start_rc = app_friction_identify_start(tolerance);
            if (start_rc != 0) {
                app_terminal_job_abort();
                const char* why = start_rc == -2 ? "motion not calibrated (or calibrating)"
                                : start_rc == -3 ? "motion still active ('motion off' first)"
                                : start_rc == -4 ? "foc not READY/RUN ('foc on' first)"
                                                 : "bad tolerance [5..100] or busy";
                csh_printf(csh, "ERR: friction cal rejected: %s\r\n", why);
                return -1;
            }
            csh_printf(
                csh,
                "friction identify: running (~26s, auto motion off at end; any key aborts)\r\n");
            return 0;
        }
        if (strcmp(action, "apply") == 0) {
            rc = app_friction_identify_apply();
            if (rc != 0)
                csh_printf(
                    csh, "ERR: friction apply rejected: %s\r\n",
                    rc == -2 ? "motion driving (stop first)" : "no valid fit (run 'cal' first)");
            else
                csh_printf(csh, "accepted: friction result applied (RAM)\r\n");
            return rc == 0 ? 0 : -1;
        }
        if (strcmp(action, "save") == 0) {
            rc = app_friction_identify_save();
            if (rc != 0)
                csh_printf(
                    csh, "ERR: friction save rejected: need FOC OFF (flash stop window)\r\n");
            else
                csh_printf(csh, "accepted: friction params saved (flash)\r\n");
            return rc;
        }
        return app_terminal_cmd_usage(csh, "motion friction show|cal|apply|save");
        csh_printf(
            csh, "%s: motion friction %s rc=%d%s%s%s\r\n", rc == 0 ? "accepted" : "ERR", action, rc,
            rc == 0 ? "" : " (", rc == 0 ? "" : app_terminal_motion_reason(rc), rc == 0 ? "" : ")");
        return rc;
    } else if (strcmp(sub, "show") == 0) {
        app_motion_snapshot_t state;
        app_motion_get_snapshot(&state);
        /* 内部为物理坐标；终端显示统一按 software.motion.output_direction 转成用户坐标。
         * iq/iq_acc 仍为 FOC 物理电流符号，便于与 foc status 对照。 */
        float out_dir = (state.output_direction < 0.0f) ? -1.0f : 1.0f;
        float shaft_user = state.shaft_rad * out_dir;
        float pos_sp_user = state.pos_setpoint_rad * out_dir;
        float speed_user = state.speed_rad_s * out_dir;
        float vel_cmd_user = state.vel_cmd_rad_s * out_dir;
        float torque_user = state.torque_nm * out_dir;
        if (shaft_user < 0.0f)
            shaft_user += 6.2831853f;
        if (pos_sp_user < 0.0f)
            pos_sp_user += 6.2831853f;
        /* 位置误差显示取模±π（跨零点不显示±2π假误差） */
        float position_error = pos_sp_user - shaft_user;
        if (position_error > 3.14159265f)
            position_error -= 6.2831853f;
        else if (position_error < -3.14159265f)
            position_error += 6.2831853f;
        csh_printf(csh, "phase trial done=%u (apply required) out_dir=%+.0f\r\n",
                   state.calibration_done, (double)out_dir);
        csh_printf(
            csh, "reject: sample=%lu duplicate=%lu vernier=%lu timing=%lu\r\n",
            (unsigned long)state.sample_failure_count, (unsigned long)state.duplicate_count,
            (unsigned long)state.vernier_failure_count, (unsigned long)state.timing_failure_count);
        csh_printf(
            csh,
            "motion: mode=%u cal=%u zero=%u valid=%u active=%u calibrating=%u samples=%lu "
            "reason=%u\r\n"
            "shaft=%.6frad speed=%.6frad/s (%.3frpm) phase=%.8fturn torque=%.4fNm iq=%.4fA "
            "good/bad=%lu/%lu\r\n"
            "exec: iq_acc=%.4fA hold=%lu lim=%u sat=%u\r\n"
            "position: sp=%.6frad vel_cmd=%.6frad/s err=%.6frad\r\n",
            (unsigned)state.mode, state.calibrated, state.zero_valid, state.feedback_valid,
            state.active, state.calibrating, (unsigned long)state.calibration_samples,
            (unsigned)state.reason, (double)shaft_user, (double)speed_user,
            (double)(speed_user * 60.0f / 6.28318530718f), (double)state.phase_turns,
            (double)torque_user, (double)state.iq_a,
            (unsigned long)state.good_count, (unsigned long)state.bad_count,
            (double)state.iq_accepted_a, (unsigned long)state.hold_count,
            (unsigned)state.current_limited, (unsigned)state.voltage_saturated,
            (double)pos_sp_user, (double)vel_cmd_user, (double)position_error);
        return 0;
    }
    if (strcmp(sub, "help") == 0) {
        return motion_usage(csh);
    }
    if (app_friction_identify_busy() && strcmp(sub, "off") != 0 && strcmp(sub, "friction") != 0
        && strcmp(sub, "show") != 0) {
        csh_printf(csh, "ERR: friction identify busy; 'motion off' aborts it\r\n");
        return -1;
    }
    if (strcmp(sub, "cal") == 0) {
        if (argc != 2)
            return motion_usage(csh);
        rc = app_motion_calibrate();
        if (rc == 0)
            csh_printf(csh, "accepted: calibration started; wait for calibration_done=1; "
                            "next: motion apply\r\n");
        else
            csh_printf(csh, "ERR: motion cal rejected (requires FOC OFF, motion idle, "
                            "valid reducer/output_ratio)\r\n");
        return rc == 0 ? 0 : -1;
    }
    if (strcmp(sub, "apply") == 0) {
        if (argc != 2)
            return motion_usage(csh);
        rc = app_motion_apply();
        if (rc == 0)
            csh_printf(csh, "accepted: phase applied (RAM); zero invalidated; "
                            "wait for valid=1, next: motion zero save\r\n");
        else
            csh_printf(csh, "ERR: motion apply rejected (requires completed calibration, "
                            "FOC OFF, unchanged motor ratio)\r\n");
        return rc == 0 ? 0 : -1;
    }
    if (strcmp(sub, "zero") == 0) {
        bool save_after = false;
        if (argc == 3) {
            if (strcmp(argv[2], "save") != 0)
                return motion_usage(csh);
            save_after = true;
        } else if (argc != 2) {
            return motion_usage(csh);
        }
        rc = app_motion_zero();
        if (rc != 0) {
            csh_printf(csh, "ERR: motion zero rejected (requires FOC OFF, calibrated, "
                            "fresh feedback)\r\n");
            return -1;
        }
        if (save_after) {
            rc = app_motion_save();
            if (rc != 0) {
                csh_printf(csh, "WARN: zero set in RAM, but flash save failed; "
                                "keep FOC OFF and retry 'motion save'\r\n");
                return -1;
            }
            csh_printf(csh, "accepted: zero set and saved to flash\r\n");
        } else {
            csh_printf(csh, "accepted: zero set in RAM only; run 'motion zero save' "
                            "or 'motion save' to persist\r\n");
        }
        return 0;
    }
    if (strcmp(sub, "save") == 0) {
        if (argc != 2)
            return motion_usage(csh);
        rc = app_motion_save();
        if (rc == 0)
            csh_printf(csh, "accepted: phase/zero saved to flash\r\n");
        else
            csh_printf(csh, "ERR: motion save rejected (requires FOC OFF, calibrated)\r\n");
        return rc == 0 ? 0 : -1;
    }
    if (strcmp(sub, "off") == 0) {
        rc = app_motion_stop();
        if (rc == 0) {
            return (foc_complete_off(csh, "motion") == 0) ? 0 : -1;
        }
    } else if (strcmp(sub, "direct") == 0)
        rc = app_foc_set_current_source(APP_FOC_CURRENT_SOURCE_DIRECT);
    else if (argc >= 3 && app_terminal_cmd_parse_float(argv[2], &value) == 0) {
        if (strcmp(sub, "torque") == 0)
            rc = app_motion_torque(value);
        else if (strcmp(sub, "speed") == 0)
            rc = app_motion_speed(value * APP_TERMINAL_FOC_RAD_PER_RPM); /* rpm → rad/s */
        else if (strcmp(sub, "pos") == 0) {
            float vel_ff = 0, torque_ff = 0;
            if ((argc >= 4 && app_terminal_cmd_parse_float(argv[3], &vel_ff) != 0)
                || (argc >= 5 && app_terminal_cmd_parse_float(argv[4], &torque_ff) != 0))
                return app_terminal_cmd_usage(
                    csh, "motion pos deg[0~360] [vel_ff_rad_s] [torque_ff_nm]");
            /* 单圈绝对值模式：0~360° → [0,2π]（360°=0°，API 取模最短路径） */
            rc = app_motion_position(value * (6.28318530718f / 360.0f), vel_ff, torque_ff);
        } else if (strcmp(sub, "pos_gains") == 0) {
            float filter_bw = 0; /* 省略=0（直通）；与 motion gains 可选参数同语义 */
            if (argc >= 4 && app_terminal_cmd_parse_float(argv[3], &filter_bw) != 0)
                return app_terminal_cmd_usage(
                    csh, "motion pos_gains pos_gain [input_filter_bandwidth] (idle)");
            rc = app_motion_position_gains(value, filter_bw);
        } else if (
            strcmp(sub, "gains") == 0 && argc >= 4
            && app_terminal_cmd_parse_float(argv[3], &ki) == 0) {
            float coulomb_nm = 0, viscous_nm_per_rad_s = 0;
            if ((argc >= 5 && app_terminal_cmd_parse_float(argv[4], &coulomb_nm) != 0)
                || (argc >= 6 && app_terminal_cmd_parse_float(argv[5], &viscous_nm_per_rad_s) != 0))
                return app_terminal_cmd_usage(
                    csh, "motion gains kp ki [coulomb_nm] [viscous] (idle)");
            rc = app_motion_gains(value, ki, coulomb_nm, viscous_nm_per_rad_s);
        } else
            return app_terminal_cmd_usage(
                csh, "motion gains kp ki [coulomb_nm] [viscous] (idle)|pos deg[0~360] "
                     "[vel_ff] [torque_ff]|pos_gains p [bw] (idle)");
    } else
        return motion_usage(csh);
    csh_printf(
        csh, "%s: motion rc=%d%s%s%s; %s\r\n", rc == 0 ? "accepted" : "ERR", rc,
        rc == 0 ? "" : " (", rc == 0 ? "" : app_terminal_motion_reason(rc),
        rc == 0 ? "" : ")", k_motion_usage);
    return rc;
}
CSH_CMD_EXPORT_ALIAS(cmd_motion, motion, );
