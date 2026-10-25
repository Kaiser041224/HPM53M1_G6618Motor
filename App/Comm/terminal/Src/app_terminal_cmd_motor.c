/**
 * @file    app_terminal_cmd_motor.c
 * @brief   Terminal 电机命令（motor / cal）—— FOC 转矩给定、电流零点标定、电角度自校准
 * @author  Kaiser
 *
 * 命令：
 *   motor iq [<A>]     （FOC q 轴电流给定；无参 = 查询）
 *   motor id [<A>]     （FOC d 轴电流给定；无参 = 查询；用于零转矩电流环测试）
 *   cal current        （电流零点标定）
 *   cal encoder        （电角度自校准：约 26s，任意键中止；结果只作数据，不自动生效）
 *   cal encoder apply  （应用最近一次成功结果：电机运行参数 + FOC 运行零点，RAM）
 *   cal encoder save   （运行参数中的零点/方向写入 flash；须 foc off）
 *   cal encoder show   （最近结果 + 当前生效零点）
 *   cal inertia        （轴端总/负载惯量辨识：默认2.0A，6点×正反转）
 *   cal inertia apply  （FOC OFF 后写 motor.load_inertia_kgm2，RAM）
 *   cal inertia save   （FOC OFF 后写 MPRM flash；storage=reset 时重启会被 YAML 覆盖）
 *   cal inertia show   （最近结果 + 当前 load_inertia）
 *   cal cogging        （齿槽/减速器角表自整定；无参数默认1/2/3rad/s、每向2圈）
 *   cal cogging show/apply/save/clear
 *   cogging on|off|rotor|output|gain|show（可由YAML默认启用）
 *
 * M1 裁剪（spec §1/§4）：V/F 开环（motor start/stop/freq/mod）、逆变桥逐相（inv）
 * 不接入；相关源文件不参与构建。
 *
 * 分层：校准推进由 app_io 任务的 app_motor_identify_run_once() 完成（与终端无关）；
 * 本文件的 cal encoder job 只做进度显示与结果打印，job 中止 = 校准中止。
 *
 * 安全联锁：驱动类命令要求无故障锁存；标定要求 FOC 关闭。
 * 命令本体快速返回，不阻塞控制环。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_terminal_cmd.h"

#include "app_terminal.h"
#include "app_terminal_job.h"

#include "app_analog_signal.h"
#include "app_foc.h"
#include "app_motor_identify.h"
#include "app_current_identify.h"
#include "app_inertia_identify.h"
#include "app_motion.h"
#include "app_cogging.h"
#include "app_cogging_identify.h"
#include "app_cogging_store.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "foc_math.h"

#include <stdio.h>
#include <string.h>

/**
 * @brief 命令 motor：iq（FOC 转矩给定）。
 *
 * 用法：
 *   motor iq [<A>]   无参 = 查询；有参 = 设置 q 轴电流
 *   motor id [<A>]   无参 = 查询；有参 = 设置 d 轴电流（零转矩测试）
 */
static int cmd_motor(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);
    const char* sub = (argc >= 2) ? argv[1] : "iq";

    if ((strcmp(sub, "help") == 0) || (strcmp(sub, "status") == 0)) {
        csh_printf(csh, "usage: motor iq [<A>] | motor id [<A>]\r\n");
        return 0;
    }

    if (strcmp(sub, "id") == 0) {
        float value;
        app_foc_current_snapshot_t snap;

        if (app_foc_get_state() == APP_FOC_STATE_CALIB) {
            csh_printf(csh, "ERR: FOC busy (calibration in progress)\r\n");
            return -1;
        }
        if ((app_foc_get_state() != APP_FOC_STATE_READY)
            && (app_foc_get_state() != APP_FOC_STATE_RUN)) {
            csh_printf(csh, "ERR: FOC not enabled (use 'foc on')\r\n");
            return -1;
        }
        if (argc >= 3) {
            if (app_terminal_cmd_parse_float(argv[2], &value) != 0) {
                csh_printf(csh, "ERR: invalid value '%s'\r\n", argv[2]);
                return -1;
            }
            /* 零转矩 d 轴测试必须同时把 q 轴给定清零，避免沿用上一条 iq 命令。 */
            if ((app_foc_set_iq_ref(0.0f) != 0) || (app_foc_set_id_ref(value) != 0)) {
                csh_printf(csh, "ERR: set id failed (requires direct current source)\r\n");
                return -1;
            }
        }
        app_foc_get_snapshot(&snap);
        csh_printf(csh, "foc id: ref=%.3f A  avg=%.3f A  now=%.3f A\r\n",
                   (double)snap.i_d_ref_a, (double)snap.i_d_avg_a, (double)snap.i_d_a);
        return 0;
    }

    if (strcmp(sub, "iq") == 0) {
        float value;
        app_foc_current_snapshot_t snap;

        if (app_foc_get_state() == APP_FOC_STATE_CALIB) {
            csh_printf(csh, "ERR: FOC busy (calibration in progress)\r\n");
            return -1;
        }
        if ((app_foc_get_state() != APP_FOC_STATE_READY)
            && (app_foc_get_state() != APP_FOC_STATE_RUN)) {
            csh_printf(csh, "ERR: FOC not enabled (use 'foc on')\r\n");
            return -1;
        }
        if (argc >= 3) {
            if (app_terminal_cmd_parse_float(argv[2], &value) != 0) {
                csh_printf(csh, "ERR: invalid value '%s'\r\n", argv[2]);
                return -1;
            }
            if (app_foc_set_iq_ref(value) != 0) {
                csh_printf(csh, "ERR: set iq failed\r\n");
                return -1;
            }
        }
        /* 回显实际生效给定（getter）与最新测量；快照为上一拍数据 */
        app_foc_get_snapshot(&snap);
        csh_printf(csh, "foc iq: ref=%.3f A  avg=%.3f A  now=%.3f A\r\n",
                   (double)app_foc_get_iq_ref(), (double)snap.i_q_avg_a, (double)snap.i_q_a);
        return 0;
    }

    csh_printf(csh, "ERR: unknown subcommand '%s'\r\n", sub);
    return app_terminal_cmd_usage(csh, "motor iq [<A>] | motor id [<A>]");
}

/**
 * @brief cal current job：1kHz 驱动非阻塞标定状态机。
 * @param now_ms 系统毫秒计数（未用）
 */
static void cal_tick(uint32_t now_ms) {
    int rc;

    (void)now_ms;

    rc = app_analog_signal_calibrate_step();
    if (rc == 1) {
        return; /* 进行中 */
    }

    if (rc == 0) {
        app_terminal_cmd_emit("\r\nOK: ADC zero calibration\r\n");
    } else {
        app_terminal_cmd_emit("\r\nFAIL: ADC zero calibration (no current required)\r\n");
    }
    app_terminal_job_abort(); /* 完成/失败：结束 job（触发 abort 回调换行+刷新） */
}

/**
 * @brief cal current job 中止回调（用户取消或完成）。
 */
static void cal_abort(void) {
    app_analog_signal_calibrate_cancel();
    app_terminal_cmd_emit("\r\n");
    app_terminal_refresh();
}

/** cal current job（静态生命周期；active 由框架维护） */
static app_terminal_job_t s_cal_job = {
    .name = "cal current",
    .tick = cal_tick,
    .abort = cal_abort,
    .active = false,
};

/* ============================================================================
 * cal encoder：电角度自校准（显示层；推进见 app_motor_identify_run_once）
 * ============================================================================ */

#define CAL_ENCODER_RAD_TO_DEG (180.0f / FOC_PI_F)

static uint32_t s_cal_encoder_beat_ms; /**< 进度刷新节拍基准 [ms] */

/**
 * @brief 校准失败原因名称
 * @param reason 原因枚举
 * @return 名称；越界返回 "?"
 */
static const char* cal_encoder_fail_name(app_identify_fail_t reason) {
    switch (reason) {
    case APP_IDENTIFY_REASON_NONE: return "none";
    case APP_IDENTIFY_REASON_TIMEOUT: return "timeout";
    case APP_IDENTIFY_REASON_DIR: return "rotor not following (dir)";
    case APP_IDENTIFY_REASON_QUALITY: return "quality low";
    case APP_IDENTIFY_REASON_RATIO: return "pole-pair/ratio mismatch";
    case APP_IDENTIFY_REASON_HYST: return "hysteresis/slip too large (check coupling)";
    case APP_IDENTIFY_REASON_NONFINITE: return "non-finite samples";
    case APP_IDENTIFY_REASON_VERIFY: return "verify failed";
    case APP_IDENTIFY_REASON_ENCODER: return "encoder error/jump";
    case APP_IDENTIFY_REASON_FAULT: return "fault";
    case APP_IDENTIFY_REASON_STATE: return "FOC state changed";
    case APP_IDENTIFY_REASON_ABORTED: return "aborted by user";
    case APP_IDENTIFY_REASON_OVERCURRENT: return "overcurrent (|i_dq| > 6A)";
    default: return "?";
    }
}

/**
 * @brief 打印校准结果明细（成功/失败共用）
 * @param result 结果
 */
static void cal_encoder_emit_result(const app_motor_identify_result_t* result) {
    app_terminal_cmd_emit("    offset=%.4f rad (%.2f deg)  end=%.2f deg  dir=%+.0f  q=%.3f  "
                          "ratio_err=%+.1f%%\r\n",
                          (double)result->offset_rad,
                          (double)(result->offset_rad * CAL_ENCODER_RAD_TO_DEG),
                          (double)(result->offset_end_rad * CAL_ENCODER_RAD_TO_DEG),
                          (double)result->direction, (double)result->quality,
                          (double)(result->ratio_err * 100.0f));
    app_terminal_cmd_emit("    fwd=%.2f rev=%.2f deg (hyst=%.2f)  probe=%+.3f/%+.3f rad "
                          "(i=%.1fA)  resid mean/max=%.2f/%.2f deg  drift=%.1f deg/s  "
                          "i_peak=%.2f A  t=%.1f s\r\n",
                          (double)(result->offset_fwd_rad * CAL_ENCODER_RAD_TO_DEG),
                          (double)(result->offset_rev_rad * CAL_ENCODER_RAD_TO_DEG),
                          (double)(foc_wrap_pm_pi(result->offset_fwd_rad - result->offset_rev_rad)
                                   * CAL_ENCODER_RAD_TO_DEG),
                          (double)result->probe_fwd_rad, (double)result->probe_rev_rad,
                          (double)result->probe_current_a, (double)result->verify_mean_deg,
                          (double)result->verify_max_deg, (double)result->verify_drift_deg_s,
                          (double)result->i_peak_a, (double)result->elapsed_ms / 1000.0);
}

/**
 * @brief cal encoder job：进度显示（200ms）与结束打印
 * @param now_ms 系统毫秒计数
 */
static void cal_encoder_tick(uint32_t now_ms) {
    app_motor_identify_result_t result;

    app_motor_identify_get_result(&result);
    if (result.active) {
        if ((uint32_t)(now_ms - s_cal_encoder_beat_ms) >= 200U) {
            uint32_t pct = (uint32_t)(result.progress * 100.0f);
            char bar[21];

            s_cal_encoder_beat_ms = now_ms;
            if (pct > 100U) {
                pct = 100U;
            }
            for (uint32_t i = 0U; i < 20U; i++) {
                bar[i] = (i < (pct / 5U)) ? '#' : '-';
            }
            bar[20] = '\0';
            app_terminal_cmd_emit("\rcal encoder [%s] %3u%%  %4.1fs  i_peak=%.2fA   ", bar,
                                  (unsigned)pct, (double)result.elapsed_ms / 1000.0,
                                  (double)result.i_peak_a);
        }
        return;
    }

    if (result.done) {
        app_terminal_cmd_emit("\r\nOK: encoder calibration (NOT applied yet)\r\n");
        cal_encoder_emit_result(&result);
        app_terminal_cmd_emit("    next: 'cal encoder apply' (RAM) -> 'foc off' -> 'cal encoder save' "
                              "(flash)\r\n");
    } else {
        app_terminal_cmd_emit("\r\nFAIL: encoder calibration (%s)\r\n",
                              cal_encoder_fail_name(result.fail_reason));
        cal_encoder_emit_result(&result);
        app_terminal_cmd_emit("    running offset unchanged; check: free rotation / encoder "
                              "mounting / pole_pairs\r\n");
    }
    app_terminal_job_abort(); /* 结束 job（abort 回调对已结束的校准为空操作） */
}

/**
 * @brief cal encoder job 中止回调（任意键 / 结束）：中止校准（非活动时为空操作）
 */
static void cal_encoder_abort(void) {
    app_motor_identify_abort();
    app_terminal_cmd_emit("\r\n");
    app_terminal_refresh();
}

/** cal encoder job（静态生命周期） */
static app_terminal_job_t s_cal_encoder_job = {
    .name = "cal encoder",
    .tick = cal_encoder_tick,
    .abort = cal_encoder_abort,
    .active = false,
};

/**
 * @brief cal encoder 子命令
 * @param csh shell 上下文
 * @param sub 子命令（NULL = 启动校准）
 * @return 0 = 成功；-1 = 失败
 */
static int cmd_cal_encoder(chry_shell_t* csh, const char* sub) {
    app_motor_identify_result_t result;

    if (sub == NULL) {
        app_foc_state_t st = app_foc_get_state();

        if (!app_terminal_cmd_require_no_fault(csh)) {
            return -1;
        }
        if ((st != APP_FOC_STATE_READY) && (st != APP_FOC_STATE_RUN)) {
            csh_printf(csh, "ERR: FOC must be READY/RUN (use 'foc on' first)\r\n");
            return -1;
        }
        csh_printf(csh, "WARN: motor must be FREE to rotate (no load); I_cal=2A d-axis; "
                        "adaptive probe 2/4A; ~26s; any key aborts\r\n");
        /* 先启动 job（会中止旧 job），再启动校准 */
        if (app_terminal_job_start(&s_cal_encoder_job) != 0) {
            csh_printf(csh, "FAIL: job start\r\n");
            return -1;
        }
        s_cal_encoder_beat_ms = 0U;
        if (app_motor_identify_start() != 0) {
            app_terminal_job_abort();
            csh_printf(csh, "FAIL: calibration start (busy / fault / FOC state / vtest)\r\n");
            return -1;
        }
        return 0;
    }

    if (strcmp(sub, "apply") == 0) {
        if (app_motor_identify_apply() != 0) {
            csh_printf(csh, "ERR: no successful calibration result to apply (run 'cal encoder')\r\n");
            return -1;
        }
        app_motor_identify_get_result(&result);
        csh_printf(csh, "OK: applied offset=%.4f rad dir=%+.0f valid=1 (RAM; 'foc off' + "
                        "'cal encoder save' to persist)\r\n",
                   (double)result.offset_rad, (double)result.direction);
        return 0;
    }

    if (strcmp(sub, "save") == 0) {
        if (app_foc_stop_pending()) {
            csh_printf(csh, "ERR: FOC stop pending; wait for 'foc: state=OFF' before flash save\r\n");
            return -1;
        }
        if (app_foc_get_state() != APP_FOC_STATE_OFF) {
            csh_printf(csh, "ERR: FOC enabled (use 'foc off' first; flash only in stop window)\r\n");
            return -1;
        }
        {
            int save_rc;

            csh_printf(csh, "saving FENC to flash (IRQ briefly off; USB may pause)...\r\n");
            save_rc = app_motor_identify_save();
            if (save_rc == APP_IDENTIFY_SAVE_PENDING) {
                csh_printf(csh, "ERR: latest calibration result not applied; run 'cal encoder "
                                "apply' first (save writes the APPLIED offset)\r\n");
                return -1;
            }
            if (save_rc != 0) {
                csh_printf(csh, "FAIL: save to flash\r\n");
                return -1;
            }
        }
        csh_printf(csh, "OK: saved offset=%.4f rad dir=%+.0f valid=%u to flash (restored at boot)\r\n",
                   (double)app_motor_params_current()->encoder.electrical_offset_rad,
                   (double)app_motor_params_current()->encoder.direction,
                   (unsigned)app_motor_params_current()->encoder.electrical_offset_valid);
        return 0;
    }

    if (strcmp(sub, "show") == 0) {
        float run_offset;
        float run_dir;
        const app_motor_params_t* motor = app_motor_params_current();

        app_foc_get_encoder_offset(&run_offset, &run_dir);
        csh_printf(csh, "encoder offset: foc(runtime)=%.4f rad dir=%+.0f | params=%.4f rad dir=%+.0f | valid=%u\r\n",
                   (double)run_offset, (double)run_dir,
                   (double)motor->encoder.electrical_offset_rad, (double)motor->encoder.direction,
                   (unsigned)motor->encoder.electrical_offset_valid);
        if (motor->encoder.electrical_offset_valid == 0U) {
            csh_printf(csh, "WARN: encoder offset uncalibrated; FOC will warn but not block\r\n");
        }
        app_motor_identify_get_result(&result);
        csh_printf(csh, "last calibration: %s\r\n",
                   result.active ? "running"
                   : result.done ? "OK (applicable)"
                   : result.failed ? cal_encoder_fail_name(result.fail_reason)
                                   : "none");
        if (result.done || result.failed) {
            cal_encoder_emit_result(&result);
        }
        return 0;
    }

    return app_terminal_cmd_usage(csh, "cal encoder [apply|save|show]");
}

/**
 * @brief 命令 cal：电流零点标定（current）/ 电角度自校准（encoder），job 驱动，不阻塞控制环。
 */
/** @brief 表格单元格式化：无记录显示 "--" */
static void cal_rl_value(char* out, size_t size, bool present, float value) {
    if (!present) {
        (void)snprintf(out, size, "--");
        return;
    }
    (void)snprintf(out, size, "%.6g", (double)value);
}

/** @brief 输出一行参数对比 */
static void cal_rl_row(const char* item, const char* yaml, const char* flash, const char* ram_now,
                       const char* ram_cal) {
    app_terminal_cmd_emit("| %-10s | %10s | %10s | %10s | %10s |\r\n", item, yaml, flash,
                          ram_now, ram_cal);
}

/** @brief Rs/Ld 与 PI 参数显示：YAML / FLASH / RAM 当前 / RAM 本次 */
static void cal_rl_emit(const app_current_identify_result_t* result) {
    const app_motor_params_t* yaml_motor = app_motor_params_default();
    const app_software_params_t* yaml_software = app_software_params_default();
    const app_motor_params_t* ram_motor = app_motor_params_current();
    const app_software_params_t* ram_software = app_software_params_current();
    app_current_identify_saved_t saved = {0};
    int saved_rc = app_current_identify_get_saved(&saved);
    bool flash_present = (saved_rc == 0) && saved.present;
    bool cal_present = result->done;
    char yaml_rs[16], yaml_ls[16], yaml_kp[16], yaml_ki[16], yaml_bw[16];
    char flash_rs[16], flash_ls[16], flash_kp[16], flash_ki[16], flash_bw[16];
    char now_rs[16], now_ls[16], now_kp[16], now_ki[16], now_bw[16];
    char cal_rs[16], cal_ls[16], cal_kp[16], cal_ki[16], cal_bw[16];

    cal_rl_value(yaml_rs, sizeof(yaml_rs), true, yaml_motor->rs_ohm);
    cal_rl_value(yaml_ls, sizeof(yaml_ls), true, yaml_motor->ls_h * 1e6f);
    cal_rl_value(yaml_kp, sizeof(yaml_kp), true, yaml_software->control.current_loop.kp);
    cal_rl_value(yaml_ki, sizeof(yaml_ki), true, yaml_software->control.current_loop.ki);
    cal_rl_value(yaml_bw, sizeof(yaml_bw), true,
                 yaml_software->control.current_loop.bandwidth_rad_s / 6.283185307f);

    cal_rl_value(flash_rs, sizeof(flash_rs), flash_present, saved.rs_ohm);
    cal_rl_value(flash_ls, sizeof(flash_ls), flash_present, saved.ld_h * 1e6f);
    cal_rl_value(flash_kp, sizeof(flash_kp), flash_present, saved.kp);
    cal_rl_value(flash_ki, sizeof(flash_ki), flash_present, saved.ki);
    cal_rl_value(flash_bw, sizeof(flash_bw), flash_present, saved.bandwidth_hz);

    cal_rl_value(now_rs, sizeof(now_rs), true, ram_motor->rs_ohm);
    cal_rl_value(now_ls, sizeof(now_ls), true, ram_motor->ls_h * 1e6f);
    cal_rl_value(now_kp, sizeof(now_kp), true, ram_software->control.current_loop.kp);
    cal_rl_value(now_ki, sizeof(now_ki), true, ram_software->control.current_loop.ki);
    cal_rl_value(now_bw, sizeof(now_bw), true,
                 ram_software->control.current_loop.bandwidth_rad_s / 6.283185307f);

    cal_rl_value(cal_rs, sizeof(cal_rs), cal_present, result->rs_ohm);
    cal_rl_value(cal_ls, sizeof(cal_ls), cal_present, result->ld_h * 1e6f);
    cal_rl_value(cal_kp, sizeof(cal_kp), cal_present, result->kp);
    cal_rl_value(cal_ki, sizeof(cal_ki), cal_present, result->ki);
    cal_rl_value(cal_bw, sizeof(cal_bw), cal_present, result->bandwidth_hz);

    app_terminal_cmd_emit("Rs/Ld / current-loop tuning (FCUR)\r\n");
    app_terminal_cmd_emit("+------------+------------+------------+------------+------------+\r\n");
    app_terminal_cmd_emit("| item       |       YAML |      FLASH |    RAM now |    RAM cal |\r\n");
    app_terminal_cmd_emit("+------------+------------+------------+------------+------------+\r\n");
    cal_rl_row("Rs [ohm]", yaml_rs, flash_rs, now_rs, cal_rs);
    cal_rl_row("Ls [uH]", yaml_ls, flash_ls, now_ls, cal_ls);
    cal_rl_row("Kp [V/A]", yaml_kp, flash_kp, now_kp, cal_kp);
    cal_rl_row("Ki [V/As]", yaml_ki, flash_ki, now_ki, cal_ki);
    cal_rl_row("bw [Hz]", yaml_bw, flash_bw, now_bw, cal_bw);
    app_terminal_cmd_emit("+------------+------------+------------+------------+------------+\r\n");
    app_terminal_cmd_emit("flash: %s | RAM cal: %s\r\n",
        (saved_rc < 0) ? "read-error" : !flash_present ? "none" : saved.valid ? "valid" : "mismatch",
        !cal_present ? "--" : result->step.passed ? "PASS" : "FAIL");
    app_terminal_cmd_emit("feedforward (RAM now): R=%u wl=%u bemf=%u | legacy_decouple=%u\r\n",
        (unsigned)ram_software->current_ff.r_en,
        (unsigned)ram_software->current_ff.wl_en,
        (unsigned)ram_software->current_ff.bemf_en,
        (unsigned)ram_software->control.current_loop.decoupling_en);
    if (!cal_present) {
        app_terminal_cmd_emit("RAM result: empty (reboot clears this snapshot; FLASH is persistent)\r\n");
    }

    /* 无活动/无结果时只显示对比表，避免重启后满屏 0。 */
    if (!result->done && !result->failed && !result->active) {
        return;
    }

    app_terminal_cmd_emit("phase zero: U=%.4f V=%.4f W=%.4f sum=%.4fA theta=%.4frad\r\n",
        (double)result->zero_phase_a[0],(double)result->zero_phase_a[1],(double)result->zero_phase_a[2],
        (double)(result->zero_phase_a[0]+result->zero_phase_a[1]+result->zero_phase_a[2]),
        (double)result->excitation_theta_rad);
    app_terminal_cmd_emit("phase driven: U=%.4f V=%.4f W=%.4f sum=%.4fA\r\n",
        (double)result->driven_phase_a[0],(double)result->driven_phase_a[1],(double)result->driven_phase_a[2],
        (double)(result->driven_phase_a[0]+result->driven_phase_a[1]+result->driven_phase_a[2]));
    static const char* const detail_names[] = {
        "none", "state", "timeout", "snapshot", "movement", "capture-invalid",
        "q-current", "d-ripple", "d-tracking", "port"
    };
    unsigned detail = (unsigned)result->detail;
    app_terminal_cmd_emit("diagnostic: stage=%u detail=%s platform=%u theta_delta=%.4frad\r\n",
        (unsigned)result->failure_stage,
        detail < sizeof(detail_names)/sizeof(detail_names[0]) ? detail_names[detail] : "unknown",
        (unsigned)result->platform_index, (double)result->theta_delta_rad);
    app_terminal_cmd_emit("capture: n=%u invalid=%u id_ref=%.3f mean=%.3f ripple=%.3fA iq_peak=%.3fA vd=%.4fV\r\n",
        (unsigned)result->capture_count, (unsigned)result->capture_invalid_count,
        (double)result->capture_ref_a, (double)result->capture_mean_a,
        (double)result->capture_ripple_a, (double)result->capture_q_peak_a,
        (double)result->capture_voltage_v);
    app_terminal_cmd_emit("steady iq_mean=%.4fA (peak is diagnostic, not acceptance criterion)\r\n",
                          (double)result->capture_q_mean_a);
    app_terminal_cmd_emit("zero-PWM baseline: id=%.4fA iq=%.4fA; excitation iq_delta=%.4fA (not subtracted)\r\n",
        (double)result->zero_d_mean_a,(double)result->zero_q_mean_a,
        (double)(result->capture_q_mean_a-result->zero_q_mean_a));
    app_terminal_cmd_emit("PI baseline: mean=%.4fA noise_rms=%.4fA (raw blocks, not filtered step)\r\n",
        (double)result->pi_baseline_a,(double)result->pi_baseline_rms_a);
    app_terminal_cmd_emit("Ld fit status=%d rejected=%.2fuH residual=%.4fA (diagnostic only)\r\n",
        result->ld_fit_status,(double)(result->ld_rejected_h*1e6f),
        (double)result->ld_rejected_residual_a);
    app_terminal_cmd_emit("Ld pulse: half4=%.2fuH half8=%.2fuH (128 samples x64..128 blocks; segment integral)\r\n",
                           (double)(result->ld_half4_h*1e6f), (double)(result->ld_half8_h*1e6f));
    app_terminal_cmd_emit("robustness: Rs half-window drift=%.4fA Ld extra_batches=%u\r\n",
        (double)result->rs_window_drift_a,(unsigned)result->ld_extra_batches);
    app_terminal_cmd_emit("PI trial: bw=%.0fHz Kp=%.5f Ki=%.3f passed=%u retry=%u rise=%.3fms overshoot=%.1f%% raw=%.1f%% error=%.4fA ripple=%.4fA rms=%.4fA\r\n",
        (double)result->trial_bandwidth_hz, (double)result->trial_kp, (double)result->trial_ki,
        (unsigned)result->trial_step.passed, (unsigned)result->bandwidth_retries,
        (double)(result->trial_step.rise_s*1000),
        (double)(result->trial_step.overshoot*100),
        (double)(result->trial_step.overshoot_raw*100), (double)result->trial_step.error_a,
        (double)result->trial_step.ripple_a, (double)result->trial_step.ripple_rms_a);
    if (result->r_ff_tested) {
        app_terminal_cmd_emit(
            "RI A/B: off rise=%.3fms overshoot=%.1f%% rms=%.4fA | on rise=%.3fms "
            "overshoot=%.1f%% rms=%.4fA | recommend=%u\r\n",
            (double)(result->r_ff_step_off.rise_s * 1000),
            (double)(result->r_ff_step_off.overshoot * 100),
            (double)result->r_ff_step_off.ripple_rms_a,
            (double)(result->r_ff_step_on.rise_s * 1000),
            (double)(result->r_ff_step_on.overshoot * 100),
            (double)result->r_ff_step_on.ripple_rms_a, (unsigned)result->r_ff_recommended);
    } else {
        app_terminal_cmd_emit("RI A/B: not tested (baseline step not passed)\r\n");
    }
    app_terminal_cmd_emit("rl: state=%u active=%u done=%u reason=%d t=%ums\r\n",
        (unsigned)result->state, (unsigned)result->active, (unsigned)result->done,
        result->reason, (unsigned)result->elapsed_ms);
    app_terminal_cmd_emit("Rs_eff=%.5f ohm Ld=%.2f uH (Lq assumed equal) Kp=%.5f Ki=%.3f bw=%.0f Hz\r\n",
        (double)result->rs_ohm, (double)(result->ld_h*1e6f), (double)result->kp,
        (double)result->ki, (double)result->bandwidth_hz);
    app_terminal_cmd_emit("fit R=%.4fV L=%.4fA repeat=%.1f%% rise=%.3fms overshoot=%.1f%% raw=%.1f%% error=%.4fA ripple=%.4fA rms=%.4fA\r\n",
        (double)result->rs_residual_v, (double)result->ld_residual_a,
        (double)(result->ld_repeat_error*100), (double)(result->step.rise_s*1000),
        (double)(result->step.overshoot*100), (double)(result->step.overshoot_raw*100),
        (double)result->step.error_a, (double)result->step.ripple_a,
        (double)result->step.ripple_rms_a);
}
/** @brief 显示job取消回调 */
static void cal_rl_abort(void) {
    app_current_identify_abort();
    app_terminal_cmd_emit("\r\n");
    app_terminal_refresh();
}
static uint32_t s_cal_rl_beat_ms;
/** @brief 仅显示层阶段名，不更改校准状态机 */
static const char* cal_rl_stage_name(app_current_identify_state_t state) {
    switch (state) {
    case APP_CURRENT_IDENTIFY_ZERO_SETTLE:
    case APP_CURRENT_IDENTIFY_ZERO_CAPTURE: return "ZERO";
    case APP_CURRENT_IDENTIFY_SETTLE:
    case APP_CURRENT_IDENTIFY_RS: return "Rs";
    case APP_CURRENT_IDENTIFY_LD_SETTLE:
    case APP_CURRENT_IDENTIFY_LD: return "Ld";
    case APP_CURRENT_IDENTIFY_PI_SETTLE:
    case APP_CURRENT_IDENTIFY_PI: return "PI";
    default: return "IDLE";
    }
}
/** @brief 显示job只消费状态，算法由app_io调度 */
static void cal_rl_tick(uint32_t now_ms) {
    app_current_identify_result_t result;
    app_current_identify_get_result(&result);
    if (result.active) {
        if (now_ms-s_cal_rl_beat_ms >= 200) {
            s_cal_rl_beat_ms = now_ms;
            /* 时间估计仅用于UI；未完成时最多99%，绝不代替算法完成标志。 */
            uint32_t pct=result.elapsed_ms/160U;
            if (pct>99U) pct=99U;
            char bar[21];
            for (unsigned i=0;i<20;++i) bar[i]=i<pct/5U ? '#' : '-';
            bar[20]='\0';
            app_terminal_cmd_emit("\rcal rl [%s] ~%2u%%  %-4s  %5.1fs   ",bar,
                                  (unsigned)pct,cal_rl_stage_name(result.state),
                                  (double)result.elapsed_ms/1000.0);
        }
        return;
    }
    if (result.done) {
        app_terminal_cmd_emit("\rcal rl [####################] 100%%  DONE  %5.1fs   \r\n",
                              (double)result.elapsed_ms/1000.0);
        app_terminal_cmd_emit("    Rs_eff=%.5f ohm  Ld=%.2f uH  half-period diff=%.1f%%\r\n",
            (double)result.rs_ohm,(double)(result.ld_h*1e6f),(double)(result.ld_repeat_error*100));
        app_terminal_cmd_emit("    PI: Kp=%.5f Ki=%.3f bw=%.0fHz retry=%u  step=%s\r\n",
            (double)result.kp,(double)result.ki,(double)result.bandwidth_hz,
            (unsigned)result.bandwidth_retries,
            result.step.passed ? "PASS" : "NOT PASSED");
        app_terminal_cmd_emit("    rise=%.3fms overshoot=%.1f%% raw=%.1f%% error=%.4fA ripple=%.4fA rms=%.4fA baseline_rms=%.4fA\r\n",
            (double)(result.step.rise_s*1000),(double)(result.step.overshoot*100),
            (double)(result.step.overshoot_raw*100),(double)result.step.error_a,
            (double)result.step.ripple_a,(double)result.step.ripple_rms_a,
            (double)result.pi_baseline_rms_a);
        if (result.r_ff_tested) {
            app_terminal_cmd_emit(
                "    RI A/B: off rise=%.3fms overshoot=%.1f%% rms=%.4fA | on rise=%.3fms "
                "overshoot=%.1f%% rms=%.4fA | recommend=%s\r\n",
                (double)(result.r_ff_step_off.rise_s * 1000),
                (double)(result.r_ff_step_off.overshoot * 100),
                (double)result.r_ff_step_off.ripple_rms_a,
                (double)(result.r_ff_step_on.rise_s * 1000),
                (double)(result.r_ff_step_on.overshoot * 100),
                (double)result.r_ff_step_on.ripple_rms_a,
                result.r_ff_recommended ? "YES" : "NO");
        } else {
            app_terminal_cmd_emit("    RI A/B: not tested (baseline step not passed)\r\n");
        }
    } else {
        app_terminal_cmd_emit("\r\nFAIL: cal rl stage=%s reason=%d detail=%u t=%.1fs\r\n",
            cal_rl_stage_name(result.failure_stage),result.reason,(unsigned)result.detail,
            (double)result.elapsed_ms/1000.0);
    }
    app_terminal_cmd_emit("    full diagnostics: cal rl show\r\n");
    if (result.done && !result.step.passed)
        app_terminal_cmd_emit("WARN: model PI calculated; step diagnostic NOT passed. Dynamic stability NOT verified.\r\n");
    app_terminal_cmd_emit(result.done ? "OK: model tuning complete (NOT applied): foc off -> cal rl apply -> cal rl save\r\n"
                                     : "FAIL: running parameters unchanged\r\n");
    app_terminal_job_abort();
}
static app_terminal_job_t s_cal_rl_job = {
    .name = "cal rl", .tick = cal_rl_tick, .abort = cal_rl_abort, .active = false
};
/** @brief 正式辨识命令入口 */
static int cmd_cal_rl(chry_shell_t* csh, const char* sub) {
    if (!sub) {
        if (app_current_identify_is_active()) return -1;
        if (app_terminal_job_start(&s_cal_rl_job)) return -1;
        if (app_current_identify_start()) {
            app_terminal_job_abort();
            csh_printf(csh, "ERR: require FOC READY; apply encoder calibration first\r\n");
            return -1;
        }
        s_cal_rl_beat_ms = 0;
        csh_printf(csh, "Rs/Ld + PI tuning started; any key cancels\r\n");
        return 0;
    }
    if (!strcmp(sub, "show")) {
        app_current_identify_result_t result;
        app_current_identify_get_result(&result);
        cal_rl_emit(&result);
        return 0;
    }
    int rc;
    if (!strcmp(sub, "apply")) {
        rc = app_current_identify_apply();
    }
    else if (!strcmp(sub, "save")) rc = app_current_identify_save();
    else if (!strcmp(sub, "clear")) rc = app_current_identify_clear();
    else return app_terminal_cmd_usage(csh, "cal rl [show|apply|save|clear]");
    if (!strcmp(sub,"apply") && rc) {
        csh_printf(csh,"apply: %s\r\n",rc==-1 ? "FOC not OFF (stop may still be pending)"
            : rc==-2 ? "no completed RAM result; run 'cal rl show' (reset loses unsaved result)"
            : rc==-4 ? "PWM frequency/pole pairs changed; recalibrate"
            : rc==-5 ? "dynamic step NOT PASSED; refuse to apply tuning/feedforward"
                     : "calibration still active");
    } else if (!strcmp(sub, "clear")) {
        csh_printf(csh, "clear: %s (FCUR removed; reboot restores YAML)\r\n",
                   rc ? "FAIL (FOC must be OFF and idle)" : "OK");
    } else csh_printf(csh, "%s: %s (save requires OFF and applied result)\r\n", sub, rc ? "FAIL" : "OK");
    return rc;
}

/* ============================================================================
 * cal inertia：轴端负载惯量辨识（直接 q 电流双窗口；FOC READY/RUN）
 * ============================================================================ */

static uint32_t s_cal_inertia_beat_ms;

/** @brief 惯量辨识失败原因文本 */
static const char* cal_inertia_fail_name(app_inertia_identify_failure_t failure) {
    switch (failure) {
    case APP_INERTIA_IDENTIFY_FAIL_STATE: return "FOC state/source changed";
    case APP_INERTIA_IDENTIFY_FAIL_FEEDBACK: return "speed feedback invalid";
    case APP_INERTIA_IDENTIFY_FAIL_TIMEOUT: return "settle/accel timeout";
    case APP_INERTIA_IDENTIFY_FAIL_SPEED: return "speed limit exceeded";
    case APP_INERTIA_IDENTIFY_FAIL_WINDOW: return "speed crossed direction";
    case APP_INERTIA_IDENTIFY_FAIL_CALC: return "calculation/range rejected";
    case APP_INERTIA_IDENTIFY_FAIL_QUALITY: return "multi-point quality rejected";
    case APP_INERTIA_IDENTIFY_FAIL_MOTION: return "motion became active";
    case APP_INERTIA_IDENTIFY_FAIL_ABORTED: return "aborted";
    default: return "none";
    }
}

/** @brief cal inertia job：进度与结果 */
static void cal_inertia_tick(uint32_t now_ms) {
    app_inertia_identify_snapshot_t identify;

    app_inertia_identify_get_snapshot(&identify);
    if (identify.busy) {
        if ((uint32_t)(now_ms - s_cal_inertia_beat_ms) >= 200U) {
            uint32_t pct = (uint32_t)(identify.progress * 100.0f);
            char bar[21];

            s_cal_inertia_beat_ms = now_ms;
            if (pct > 100U)
                pct = 100U;
            for (uint32_t i = 0U; i < 20U; i++)
                bar[i] = (i < (pct / 5U)) ? '#' : '-';
            bar[20] = '\0';
            app_terminal_cmd_emit(
                "\rcal inertia [%s] %3u%%  %4.1fs  run=%u/%u valid=%u  state=%u  |v|=%.2f  ",
                bar, (unsigned)pct, (double)identify.elapsed_ms / 1000.0,
                (unsigned)identify.run_index, (unsigned)identify.run_count,
                (unsigned)identify.valid_runs, (unsigned)identify.state,
                (double)identify.speed_peak_rad_s);
        }
        return;
    }
    app_terminal_cmd_emit("\r\n%s: inertia identify (%s)\r\n",
                          identify.result_valid ? "OK" : "FAIL",
                          cal_inertia_fail_name(identify.failure));
    app_terminal_cmd_emit(
        "    J_total(median)=%.5f kgm2  J_rotor_ref=%.5f  J_load=%.5f%s\r\n",
        (double)identify.result.j_total_kgm2, (double)identify.result.j_rotor_reflected_kgm2,
        (double)identify.result.j_load_kgm2, identify.result.clamped ? " (load clamped 0)" : "");
    app_terminal_cmd_emit(
        "    points=%u  runs=%u/%u valid (F=%u R=%u)  median=%.5f  MAD=%.5f  spread=%.1f%%\r\n",
        (unsigned)identify.point_count, (unsigned)identify.valid_runs,
        (unsigned)identify.run_count, (unsigned)identify.valid_forward_runs,
        (unsigned)identify.valid_reverse_runs, (double)identify.median_j_total_kgm2,
        (double)identify.mad_j_total_kgm2, (double)(identify.spread_ratio * 100.0f));
    for (uint16_t i = 0U; i < identify.run_count; i++) {
        bool valid = (identify.run_valid_mask & (uint16_t)(1U << i)) != 0U;

        app_terminal_cmd_emit(
            "    pt%u%c: I=%.2fA v=%.2f..%.2f  J_total=%s%.5f kgm2\r\n",
            (unsigned)(i / 2U + 1U), identify.run_direction[i] > 0 ? 'F' : 'R',
            (double)identify.run_current[i], (double)identify.run_start_speed[i],
            (double)identify.run_stop_speed[i], valid ? "" : "INVALID ",
            (double)identify.run_j_total[i]);
    }
    if (identify.result_valid) {
        app_terminal_cmd_emit(
            "    next: 'foc off' -> 'cal inertia apply' (RAM) -> 'cal inertia save' (flash)\r\n");
        if (app_software_params_default()->storage == APP_SW_STORAGE_RESET) {
            app_terminal_cmd_emit(
                "    note: storage=reset -> 'save' is overwritten at boot; persist via "
                "motor.yaml or storage:keep\r\n");
        }
    } else {
        app_terminal_cmd_emit("    running motor.load_inertia unchanged\r\n");
    }
    app_terminal_job_abort();
}

/** @brief cal inertia job 中止回调 */
static void cal_inertia_abort(void) {
    app_inertia_identify_abort();
    app_terminal_cmd_emit("\r\n");
    app_terminal_refresh();
}

/** cal inertia job（静态生命周期） */
static app_terminal_job_t s_cal_inertia_job = {
    .name = "cal inertia",
    .tick = cal_inertia_tick,
    .abort = cal_inertia_abort,
    .active = false,
};

/** @brief 启动 cal inertia；current_a<=0 使用模块默认值 */
static int cal_inertia_start(chry_shell_t* csh, float current_a) {
    app_inertia_identify_snapshot_t identify;

    if (!app_terminal_cmd_require_no_fault(csh)) {
        return -1;
    }
    if (app_foc_get_state() != APP_FOC_STATE_READY
        && app_foc_get_state() != APP_FOC_STATE_RUN) {
        csh_printf(csh, "ERR: FOC must be READY/RUN (use 'foc on' first)\r\n");
        return -1;
    }
    if (app_terminal_job_start(&s_cal_inertia_job) != 0) {
        csh_printf(csh, "FAIL: job start\r\n");
        return -1;
    }
    s_cal_inertia_beat_ms = 0U;
    int rc = app_inertia_identify_start(current_a);
    if (rc != 0) {
        app_terminal_job_abort();
        csh_printf(csh, "ERR: inertia identify rejected: %s\r\n",
                   rc == -2 ? "FOC not READY/RUN"
                   : rc == -3 ? "motion active/calibrating"
                   : rc == -4 ? "another identify/vtest active"
                   : rc == -5 ? "cannot claim DIRECT current source"
                              : "parameter/busy");
        return -1;
    }
    app_inertia_identify_get_snapshot(&identify);
    csh_printf(csh,
               "WARN: axis must be free to rotate; load inertia is INCLUDED; "
               "multi-point max I=%.2fA; %u points x 2 directions; any key aborts\r\n",
               (double)identify.max_current_a, (unsigned)identify.point_count);
    return 0;
}

/** @brief cal inertia 子命令 */
static int cmd_cal_inertia(chry_shell_t* csh, const char* sub, const char* current_text) {
    app_inertia_identify_snapshot_t identify;

    if (sub == NULL) {
        return cal_inertia_start(csh, 0.0f);
    }
    if ((strcmp(sub, "show") != 0) && (strcmp(sub, "apply") != 0)
        && (strcmp(sub, "save") != 0)) {
        float current_a = 0.0f;

        if ((current_text != NULL) || (app_terminal_cmd_parse_float(sub, &current_a) != 0)) {
            return app_terminal_cmd_usage(csh, "cal inertia [max_current_a|show|apply|save]");
        }
        return cal_inertia_start(csh, current_a);
    }

    if (strcmp(sub, "show") == 0) {
        app_inertia_identify_get_snapshot(&identify);
        csh_printf(csh,
                   "inertia: state=%u failure=%u valid=%u applied=%u runs=%u/%u\r\n"
                   "result: J_total=%.5f kgm2 J_rotor_ref=%.5f J_load=%.5f clamped=%u\r\n"
                   "quality: F=%u R=%u median=%.5f MAD=%.5f spread=%.1f%%\r\n"
                   "params: load_inertia=%.5f kgm2\r\n",
                   (unsigned)identify.state, (unsigned)identify.failure,
                   (unsigned)identify.result_valid, (unsigned)identify.applied,
                   (unsigned)identify.valid_runs, (unsigned)identify.run_count,
                   (double)identify.result.j_total_kgm2,
                   (double)identify.result.j_rotor_reflected_kgm2,
                   (double)identify.result.j_load_kgm2,
                   (unsigned)identify.result.clamped,
                   (unsigned)identify.valid_forward_runs,
                   (unsigned)identify.valid_reverse_runs,
                   (double)identify.median_j_total_kgm2,
                   (double)identify.mad_j_total_kgm2,
                   (double)(identify.spread_ratio * 100.0f),
                   (double)app_motor_params_current()->load_inertia_kgm2);
        return 0;
    }
    if (strcmp(sub, "apply") == 0) {
        int rc = app_inertia_identify_apply();
        if (rc != 0) {
            csh_printf(csh, "ERR: inertia apply rejected: %s\r\n",
                       rc == -2 ? "FOC not OFF (run 'foc off')" : "no valid result");
            return -1;
        }
        if (app_software_params_default()->storage == APP_SW_STORAGE_RESET) {
            csh_printf(
                csh,
                "OK: applied load_inertia=%.5f kgm2 (RAM; storage=reset -> update motor.yaml to persist)\r\n",
                (double)app_motor_params_current()->load_inertia_kgm2);
        } else {
            csh_printf(csh, "OK: applied load_inertia=%.5f kgm2 (RAM; save to persist)\r\n",
                       (double)app_motor_params_current()->load_inertia_kgm2);
        }
        return 0;
    }
    if (strcmp(sub, "save") == 0) {
        int rc = app_inertia_identify_save();
        if (rc != 0) {
            csh_printf(csh, "ERR: inertia save rejected: need applied result and FOC OFF\r\n");
            return -1;
        }
        csh_printf(csh, "OK: load_inertia saved to flash\r\n");
        return 0;
    }
    return app_terminal_cmd_usage(csh, "cal inertia [max_current_a|show|apply|save]");
}


/* ============================================================================
 * cal cogging：齿槽/减速器输出角表自整定
 * ============================================================================ */
static uint32_t s_cal_cogging_beat_ms;
static uint32_t s_cal_cogging_start_ms;
static bool s_cal_cogging_arm;

static const char* cal_cogging_fail_name(app_cogging_identify_failure_t failure) {
    switch (failure) {
    case APP_COGGING_IDENTIFY_FAIL_STATE: return "FOC state changed";
    case APP_COGGING_IDENTIFY_FAIL_MOTION: return "motion aborted/rejected";
    case APP_COGGING_IDENTIFY_FAIL_FEEDBACK: return "encoder feedback invalid";
    case APP_COGGING_IDENTIFY_FAIL_TIMEOUT: return "settle/stop timeout";
    case APP_COGGING_IDENTIFY_FAIL_SAMPLE: return "speed/sample window invalid";
    case APP_COGGING_IDENTIFY_FAIL_QUALITY: return "table quality/coverage rejected";
    case APP_COGGING_IDENTIFY_FAIL_ABORTED: return "aborted";
    case APP_COGGING_IDENTIFY_FAIL_BUSY: return "another identify active";
    default: return "none";
    }
}

static void cal_cogging_tick(uint32_t now_ms) {
    app_cogging_identify_snapshot_t identify;

    app_cogging_identify_get_snapshot(&identify);
    if (identify.busy) {
        if (!s_cal_cogging_arm) {
            s_cal_cogging_start_ms = now_ms;
            s_cal_cogging_arm = true;
        }
        /* 独立于状态机的 watchdog：状态机若未被调度，必须自动停止运动。 */
        if (((identify.samples == 0U)
             && ((uint32_t)(now_ms - s_cal_cogging_start_ms) > 10000U))
            || ((uint32_t)(now_ms - s_cal_cogging_start_ms) > 300000U)) {
            app_cogging_identify_abort();
            app_terminal_cmd_emit("\r\nFAIL: cogging watchdog (no progress)\r\n");
            app_terminal_job_abort();
            return;
        }
        if ((uint32_t)(now_ms - s_cal_cogging_beat_ms) >= 200U) {
            uint32_t pct = (uint32_t)(identify.progress * 100.0f);
            char bar[21];

            s_cal_cogging_beat_ms = now_ms;
            if (pct > 100U) pct = 100U;
            for (uint32_t i = 0U; i < 20U; i++) bar[i] = (i < (pct / 5U)) ? '#' : '-';
            bar[20] = '\0';
            app_terminal_cmd_emit(
                "\rcal cogging [%s] %3u%%  %4.1fs  %u samples  v=%.2f  pt=%u/%u  "
                "t=%.2f  state=%u  ",
                bar, (unsigned)pct, (double)identify.elapsed_ms / 1000.0,
                (unsigned)identify.samples, (double)identify.speed_rad_s,
                (unsigned)(identify.point_index + 1U), (unsigned)identify.point_count,
                (double)identify.travel_rad, (unsigned)identify.state);
        }
        return;
    }

    s_cal_cogging_arm = false;
    app_terminal_cmd_emit("\r\n%s: cogging identify (%s)\r\n",
                          identify.result_valid ? "OK" : "FAIL",
                          cal_cogging_fail_name(identify.failure));
    for (uint8_t i = 0U; i < identify.point_count; i++) {
        const app_cogging_identify_point_t* point = &identify.points[i];
        app_terminal_cmd_emit(
            "    pt%u v=%.2f rad/s revs=%u | F mean=%.3f rms=%.3f n=%u | "
            "R mean=%.3f rms=%.3f n=%u\r\n",
            (unsigned)(i + 1U), (double)point->speed_rad_s, (unsigned)point->revs,
            (double)point->fwd_iq_mean_a, (double)point->fwd_iq_rms_a,
            (unsigned)point->fwd_samples, (double)point->rev_iq_mean_a,
            (double)point->rev_iq_rms_a, (unsigned)point->rev_samples);
    }
    if (identify.result_valid) {
        app_terminal_cmd_emit(
            "    rotor: %s rms=%.5f Nm pp=%.5f bins=%u\r\n"
            "    output: %s rms=%.5f Nm pp=%.5f bins=%u\r\n"
            "    samples=%u  next: 'foc off' -> 'cal cogging apply' -> "
            "'cal cogging save' -> 'cogging on' (or YAML default)\r\n",
            identify.rotor_valid ? "OK" : "INVALID", (double)identify.rotor_stats.rms_nm,
            (double)identify.rotor_stats.peak_to_peak_nm,
            (unsigned)identify.rotor_stats.valid_bins, identify.output_valid ? "OK" : "INVALID",
            (double)identify.output_stats.rms_nm,
            (double)identify.output_stats.peak_to_peak_nm,
            (unsigned)identify.output_stats.valid_bins, (unsigned)identify.samples);
    } else {
        app_terminal_cmd_emit("    active tables unchanged\r\n");
    }
    app_terminal_job_abort();
}

static void cal_cogging_abort(void) {
    app_cogging_identify_abort();
    app_terminal_cmd_emit("\r\n");
    app_terminal_refresh();
}

static app_terminal_job_t s_cal_cogging_job = {
    .name = "cal cogging",
    .tick = cal_cogging_tick,
    .abort = cal_cogging_abort,
    .active = false,
};

static int cal_cogging_start(chry_shell_t* csh, float speed_rad_s, uint32_t revs) {
    app_cogging_identify_snapshot_t identify;

    if (!app_terminal_cmd_require_no_fault(csh)) return -1;
    if (app_foc_get_state() != APP_FOC_STATE_READY
        && app_foc_get_state() != APP_FOC_STATE_RUN) {
        csh_printf(csh, "ERR: FOC must be READY/RUN (use 'foc on' first)\r\n");
        return -1;
    }
    if (app_terminal_job_start(&s_cal_cogging_job) != 0) {
        csh_printf(csh, "FAIL: job start\r\n");
        return -1;
    }
    s_cal_cogging_beat_ms = 0U;
    s_cal_cogging_start_ms = 0U;
    s_cal_cogging_arm = false;
    int rc = app_cogging_identify_start(speed_rad_s, (uint16_t)revs);
    if (rc != 0) {
        app_terminal_job_abort();
        csh_printf(csh, "ERR: cogging identify rejected: %s\r\n",
                   rc == -2 ? "FOC not READY/RUN"
                   : rc == -3 ? "motion not calibrated/active"
                   : rc == -4 ? "another identify active"
                   : rc == -5 ? "motion command failed"
                              : "parameter/busy");
        return -1;
    }
    app_cogging_identify_get_snapshot(&identify);
    csh_printf(csh,
               "WARN: axis must be free; motor body + reducer only; no external load. "
               "points=%u:", (unsigned)identify.point_count);
    for (uint8_t i = 0U; i < identify.point_count; i++) {
        csh_printf(csh, " %.2f@%u", (double)identify.points[i].speed_rad_s,
                   (unsigned)identify.points[i].revs);
    }
    csh_printf(csh, " rad/s@revs; any key aborts; result not auto-enabled\r\n");
    return 0;
}

static int cal_cogging_show(chry_shell_t* csh) {
    app_cogging_identify_snapshot_t identify;
    app_cogging_snapshot_t active;

    app_cogging_identify_get_snapshot(&identify);
    app_cogging_get_snapshot(&active);
    csh_printf(csh,
               "cogging identify: state=%u failure=%u valid=%u samples=%u\r\n"
               "rotor: valid=%u rms=%.5f pp=%.5f bins=%u\r\n"
               "output: valid=%u rms=%.5f pp=%.5f bins=%u\r\n"
               "active: master=%u rotor=%u output=%u rvalid=%u ovalid=%u gain=%.3f\r\n"
               "active rotor: rms=%.5f pp=%.5f bins=%u\r\n"
               "active output: rms=%.5f pp=%.5f bins=%u\r\n",
               (unsigned)identify.state, (unsigned)identify.failure,
               (unsigned)identify.result_valid, (unsigned)identify.samples,
               (unsigned)identify.rotor_valid, (double)identify.rotor_stats.rms_nm,
               (double)identify.rotor_stats.peak_to_peak_nm,
               (unsigned)identify.rotor_stats.valid_bins, (unsigned)identify.output_valid,
               (double)identify.output_stats.rms_nm,
               (double)identify.output_stats.peak_to_peak_nm,
               (unsigned)identify.output_stats.valid_bins, (unsigned)active.master_enabled,
               (unsigned)active.rotor_enabled, (unsigned)active.output_enabled,
               (unsigned)active.rotor_valid, (unsigned)active.output_valid,
               (double)active.gain, (double)active.rotor_stats.rms_nm,
               (double)active.rotor_stats.peak_to_peak_nm,
               (unsigned)active.rotor_stats.valid_bins, (double)active.output_stats.rms_nm,
               (double)active.output_stats.peak_to_peak_nm,
               (unsigned)active.output_stats.valid_bins);
    for (uint8_t i = 0U; i < identify.point_count; i++) {
        const app_cogging_identify_point_t* point = &identify.points[i];
        csh_printf(csh,
                   "point%u: v=%.2f revs=%u | F mean=%.3f rms=%.3f n=%u | "
                   "R mean=%.3f rms=%.3f n=%u\r\n",
                   (unsigned)(i + 1U), (double)point->speed_rad_s, (unsigned)point->revs,
                   (double)point->fwd_iq_mean_a, (double)point->fwd_iq_rms_a,
                   (unsigned)point->fwd_samples, (double)point->rev_iq_mean_a,
                   (double)point->rev_iq_rms_a, (unsigned)point->rev_samples);
    }
    return 0;
}

static int cmd_cal_cogging(chry_shell_t* csh, const char* sub, const char* revs_text) {
    if (sub == NULL) return cal_cogging_start(csh, 0.0f, 0U);

    if (strcmp(sub, "show") == 0) return cal_cogging_show(csh);
    if (strcmp(sub, "apply") == 0) {
        int rc = app_cogging_identify_apply();
        if (rc != 0) {
            csh_printf(csh, "ERR: cogging apply rejected (need valid result + FOC OFF)\r\n");
            return -1;
        }
        app_cogging_snapshot_t snapshot;
        app_cogging_get_snapshot(&snapshot);
        if (snapshot.master_enabled) {
            csh_printf(csh,
                       "OK: cogging tables applied; YAML default enabled "
                       "(rotor=%u output=%u gain=%.3f)\r\n",
                       (unsigned)snapshot.rotor_enabled, (unsigned)snapshot.output_enabled,
                       (double)snapshot.gain);
        } else {
            csh_printf(csh,
                       "OK: cogging tables applied; enable with 'cogging on' "
                       "(default master off; rotor=%u output=%u)\r\n",
                       (unsigned)snapshot.rotor_enabled, (unsigned)snapshot.output_enabled);
        }
        return 0;
    }
    if (strcmp(sub, "save") == 0) {
        int rc = app_cogging_store_save();
        if (rc != 0) {
            csh_printf(csh, "ERR: cogging save rejected (need applied tables + FOC OFF)\r\n");
            return -1;
        }
        csh_printf(csh, "OK: cogging tables saved to flash\r\n");
        return 0;
    }
    if (strcmp(sub, "clear") == 0) {
        int rc = app_cogging_store_clear();
        if (rc != 0) {
            csh_printf(csh, "ERR: cogging clear rejected (need FOC OFF)\r\n");
            return -1;
        }
        csh_printf(csh, "OK: cogging tables cleared\r\n");
        return 0;
    }

    float speed_rad_s = 0.0f;
    uint32_t revs = 0U;
    if (app_terminal_cmd_parse_float(sub, &speed_rad_s) != 0
        || (revs_text != NULL && app_terminal_cmd_parse_u32(revs_text, &revs) != 0)) {
        return app_terminal_cmd_usage(csh, "cal cogging [speed_rad_s] [revs]|show|apply|save|clear");
    }
    return cal_cogging_start(csh, speed_rad_s, revs);
}

/** @brief cogging 运行时使能/查询；默认全关 */
static int cmd_cogging(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);
    app_cogging_snapshot_t snapshot;

    app_cogging_get_snapshot(&snapshot);
    if (argc < 2 || strcmp(argv[1], "show") == 0) {
        csh_printf(csh,
                   "cogging: master=%u rotor=%u output=%u rvalid=%u ovalid=%u gain=%.3f "
                   "suspended=%u speed_limited=%u\r\n",
                   (unsigned)snapshot.master_enabled, (unsigned)snapshot.rotor_enabled,
                   (unsigned)snapshot.output_enabled, (unsigned)snapshot.rotor_valid,
                   (unsigned)snapshot.output_valid, (double)snapshot.gain,
                   (unsigned)snapshot.suspended, (unsigned)snapshot.speed_limited);
        csh_printf(csh, "usage: cogging on|off|rotor on|off|output on|off|gain <0..2>|show\r\n");
        return 0;
    }
    if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0) {
        bool enable = strcmp(argv[1], "on") == 0;
        if (enable) {
            app_motion_snapshot_t motion;
            app_motion_get_snapshot(&motion);
            if (motion.active || motion.calibrating) {
                csh_printf(csh, "ERR: stop motion before enabling cogging\r\n");
                return -1;
            }
        }
        int rc = app_cogging_set_master(enable);
        csh_printf(csh, "%s: master rc=%d\r\n", rc == 0 ? "OK" : "ERR", rc);
        return rc;
    }
    if (argc >= 3 && (strcmp(argv[1], "rotor") == 0 || strcmp(argv[1], "output") == 0)
        && (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0)) {
        bool enable = strcmp(argv[2], "on") == 0;
        if (enable) {
            app_motion_snapshot_t motion;
            app_motion_get_snapshot(&motion);
            if (motion.active || motion.calibrating) {
                csh_printf(csh, "ERR: stop motion before enabling cogging table\r\n");
                return -1;
            }
        }
        int rc = (argv[1][0] == 'r') ? app_cogging_set_rotor_enable(enable)
                                     : app_cogging_set_output_enable(enable);
        csh_printf(csh, "%s: %s rc=%d\r\n", rc == 0 ? "OK" : "ERR", argv[1], rc);
        return rc;
    }
    if (argc >= 3 && strcmp(argv[1], "gain") == 0) {
        float gain = 0.0f;
        if (app_terminal_cmd_parse_float(argv[2], &gain) != 0) {
            return app_terminal_cmd_usage(csh, "cogging gain <0..2>");
        }
        int rc = app_cogging_set_gain(gain);
        csh_printf(csh, "%s: gain rc=%d\r\n", rc == 0 ? "OK" : "ERR", rc);
        return rc;
    }
    return app_terminal_cmd_usage(csh, "cogging on|off|rotor on|off|output on|off|gain|show");
}

static int cmd_cal(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);

    if ((argc >= 2) && !strcmp(argv[1], "rl"))
        return cmd_cal_rl(csh, argc >= 3 ? argv[2] : NULL);

    if ((argc >= 2) && (strcmp(argv[1], "encoder") == 0)) {
        return cmd_cal_encoder(csh, (argc >= 3) ? argv[2] : NULL);
    }
    if ((argc >= 2) && (strcmp(argv[1], "inertia") == 0)) {
        return cmd_cal_inertia(csh, (argc >= 3) ? argv[2] : NULL,
                                (argc >= 4) ? argv[3] : NULL);
    }
    if ((argc >= 2) && (strcmp(argv[1], "cogging") == 0)) {
        return cmd_cal_cogging(csh, (argc >= 3) ? argv[2] : NULL,
                               (argc >= 4) ? argv[3] : NULL);
    }
    if ((argc < 2) || (strcmp(argv[1], "current") != 0)) {
        return app_terminal_cmd_usage(csh,
                                      "cal current | cal encoder [apply|save|show] | "
                                      "cal inertia [max_current_a|apply|save|show] | "
                                      "cal cogging [speed_rad_s] [revs]|apply|save|show|clear | "
                                      "cal rl [apply|save|show|clear]");
    }
    if (!app_terminal_cmd_require_motor_stopped(csh)) {
        return -1;
    }
    if (!app_terminal_cmd_require_no_fault(csh)) {
        return -1;
    }
    if (app_foc_get_state() != APP_FOC_STATE_OFF) {
        csh_printf(csh, "ERR: FOC enabled (use 'foc off' first)\r\n");
        return -1;
    }

    /* 先启动 job（内部会中止旧 job → 旧 cal 的 abort 回调取消旧标定），再启动新标定 */
    if (app_terminal_job_start(&s_cal_job) != 0) {
        csh_printf(csh, "FAIL: job start\r\n");
        return -1;
    }
    if (app_analog_signal_calibrate_start() != 0) {
        app_terminal_job_abort();
        csh_printf(csh, "FAIL: calibration start\r\n");
        return -1;
    }

    csh_printf(csh, "calibrating current zero (256 frames, any key to cancel)...\r\n");
    return 0;
}

CSH_CMD_EXPORT_ALIAS(cmd_motor, motor, );
CSH_CMD_EXPORT_ALIAS(cmd_cal, cal, );
CSH_CMD_EXPORT_ALIAS(cmd_cogging, cogging, );
