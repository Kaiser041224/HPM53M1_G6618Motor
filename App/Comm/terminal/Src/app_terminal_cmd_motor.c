/**
 * @file    app_terminal_cmd_motor.c
 * @brief   Terminal 电机命令（motor / cal）—— FOC 转矩给定、电流零点标定、电角度自校准
 * @author  Kaiser
 *
 * 命令：
 *   motor iq [<A>]     （FOC 转矩给定；无参 = 查询）
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
#include "app_motor_params.h"
#include "foc_math.h"

#include <string.h>

/**
 * @brief 命令 motor：iq（FOC 转矩给定）。
 *
 * 用法：
 *   motor [iq [<A>]]   无参 = 查询；有参 = 设置（限幅 ±i_q_max）
 */
static int cmd_motor(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);
    const char* sub = (argc >= 2) ? argv[1] : "iq";

    if ((strcmp(sub, "help") == 0) || (strcmp(sub, "status") == 0)) {
        csh_printf(csh, "usage: motor iq [<A>]   (FOC torque ref; no arg = query)\r\n");
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
    return app_terminal_cmd_usage(csh, "motor iq [<A>]");
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
    app_terminal_cmd_emit("    offset=%.4f rad (%.2f deg)  dir=%+.0f  q=%.3f  ratio_err=%+.1f%%\r\n",
                          (double)result->offset_rad,
                          (double)(result->offset_rad * CAL_ENCODER_RAD_TO_DEG),
                          (double)result->direction, (double)result->quality,
                          (double)(result->ratio_err * 100.0f));
    app_terminal_cmd_emit("    fwd=%.2f rev=%.2f deg (hyst=%.2f)  probe=%.3f rad  resid mean/max=%.2f/%.2f "
                          "deg  i_peak=%.2f A  t=%.1f s\r\n",
                          (double)(result->offset_fwd_rad * CAL_ENCODER_RAD_TO_DEG),
                          (double)(result->offset_rev_rad * CAL_ENCODER_RAD_TO_DEG),
                          (double)(foc_wrap_pm_pi(result->offset_fwd_rad - result->offset_rev_rad)
                                   * CAL_ENCODER_RAD_TO_DEG),
                          (double)result->probe_travel_rad, (double)result->verify_mean_deg,
                          (double)result->verify_max_deg, (double)result->i_peak_a,
                          (double)result->elapsed_ms / 1000.0);
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
        if (app_foc_get_state() != APP_FOC_STATE_OFF) {
            csh_printf(csh, "ERR: FOC enabled (use 'foc off' first; flash only in stop window)\r\n");
            return -1;
        }
        {
            int save_rc = app_motor_identify_save();

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
static int cmd_cal(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);

    if ((argc >= 2) && (strcmp(argv[1], "encoder") == 0)) {
        return cmd_cal_encoder(csh, (argc >= 3) ? argv[2] : NULL);
    }
    if ((argc < 2) || (strcmp(argv[1], "current") != 0)) {
        return app_terminal_cmd_usage(csh, "cal current | cal encoder [apply|save|show]");
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
