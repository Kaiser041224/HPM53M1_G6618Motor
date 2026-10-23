/**
 * @file    app_terminal_cmd_motor.c
 * @brief   Terminal 电机命令（motor / cal）—— FOC 转矩给定、电流零点标定、电角度自校准
 * @author  Kaiser
 *
 * 命令：
 *   motor iq [<A>]     （FOC q 轴电流给定；无参 = 查询）
 *   motor id [<A>]     （FOC d 轴电流给定；无参 = 查询；用于零转矩电流环测试）
 *   cal current        （电流零点标定）
 *   cal encoder        （电角度自校准：约 19s，任意键中止；结果只作数据，不自动生效）
 *   cal encoder apply  （应用最近一次成功结果：电机运行参数 + FOC 运行零点，RAM）
 *   cal encoder save   （运行参数中的零点/方向写入 flash；须 foc off）
 *   cal encoder show   （最近结果 + 当前生效零点）
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
    app_terminal_cmd_emit("    fwd=%.2f rev=%.2f deg (hyst=%.2f)  probe=%.3f rad  resid mean/max=%.2f/%.2f "
                          "deg  drift=%.1f deg/s  i_peak=%.2f A  t=%.1f s\r\n",
                          (double)(result->offset_fwd_rad * CAL_ENCODER_RAD_TO_DEG),
                          (double)(result->offset_rev_rad * CAL_ENCODER_RAD_TO_DEG),
                          (double)(foc_wrap_pm_pi(result->offset_fwd_rad - result->offset_rev_rad)
                                   * CAL_ENCODER_RAD_TO_DEG),
                          (double)result->probe_travel_rad, (double)result->verify_mean_deg,
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
        csh_printf(csh, "WARN: motor must be FREE to rotate (no load); I_cal=2A d-axis; ~19s; "
                        "any key aborts\r\n");
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
        csh_printf(csh, "OK: applied offset=%.4f rad dir=%+.0f (RAM; 'foc off' + 'cal encoder save' "
                        "to persist)\r\n",
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
        csh_printf(csh, "OK: saved offset=%.4f rad dir=%+.0f to flash (restored at boot)\r\n",
                   (double)app_motor_params_current()->encoder.electrical_offset_rad,
                   (double)app_motor_params_current()->encoder.direction);
        return 0;
    }

    if (strcmp(sub, "show") == 0) {
        float run_offset;
        float run_dir;
        const app_motor_params_t* motor = app_motor_params_current();

        app_foc_get_encoder_offset(&run_offset, &run_dir);
        csh_printf(csh, "encoder offset: foc(runtime)=%.4f rad dir=%+.0f | params=%.4f rad dir=%+.0f\r\n",
                   (double)run_offset, (double)run_dir,
                   (double)motor->encoder.electrical_offset_rad, (double)motor->encoder.direction);
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
static int cmd_cal(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);

    if ((argc >= 2) && !strcmp(argv[1], "rl"))
        return cmd_cal_rl(csh, argc >= 3 ? argv[2] : NULL);

    if ((argc >= 2) && (strcmp(argv[1], "encoder") == 0)) {
        return cmd_cal_encoder(csh, (argc >= 3) ? argv[2] : NULL);
    }
    if ((argc < 2) || (strcmp(argv[1], "current") != 0)) {
        return app_terminal_cmd_usage(csh, "cal current | cal encoder [apply|save|show] | cal rl [apply|save|show|clear]");
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
