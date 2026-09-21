/**
 * @file    app_terminal_cmd_motor.c
 * @brief   Terminal 电机命令（motor / inv / cal）
 * @author  Kaiser
 *
 * 命令：
 *   motor start | stop | freq <+|-> | mod <+|->   （开环 V/F 自检）
 *   motor iq [<A>]                               （FOC 转矩给定；无参 = 查询）
 *   inv <u|v|w|all|off>                          （三相逆变桥逐相输出）
 *   cal current                                  （电流零点标定）
 *   cal encoder                                  （电角度辨识；job 驱动，结果落 RAM）
 *
 * 安全联锁（设计文档 §8）：
 *   驱动类命令要求无故障锁存；先停旋转；标定要求电机停止。
 *   inv 使能 / cal current 与 FOC 互斥（需先 'foc off'）；inv off 始终允许。
 *   命令本体快速返回，不阻塞控制环。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_terminal_cmd.h"

#include "app_terminal.h"
#include "app_terminal_job.h"

#include "app_analog_signal.h"
#include "app_debug_inverter.h"
#include "app_debug_motor.h"
#include "app_foc.h"
#include "app_motor_identify.h"
#include "app_motor_params.h"
#include "foc_math.h"

#include <string.h>

/**
 * @brief 打印电机状态行（运行态 / 电频率 / 调制比）。
 * @param csh terminal 实例
 */
static void motor_print_status(chry_shell_t* csh) {
    float freq_hz = 0.0f;
    float mod = 0.0f;
    bool running = false;

    app_debug_motor_get_state(&freq_hz, &mod, &running);
    csh_printf(csh, "motor: %s  freq=%.2f Hz  mod=%.1f%%\r\n", running ? "RUN " : "STOP",
               (double)freq_hz, (double)(mod * 100.0f));
}

/**
 * @brief 命令 motor：status / start / stop / freq / mod。
 *
 * 用法：
 *   motor [status]              查询状态
 *   motor start | stop          启停旋转
 *   motor freq [<hz>|+|-]       电频率（0.5~10 Hz；无参=查询）
 *   motor mod  [<pct>|+|-]      调制比（1~10 %；无参=查询）
 */
static int cmd_motor(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);
    const char* sub;

    if (argc < 2) {
        motor_print_status(csh);
        return 0;
    }

    sub = argv[1];

    if (strcmp(sub, "status") == 0) {
        motor_print_status(csh);
        return 0;
    }

    if (strcmp(sub, "help") == 0) {
        csh_printf(csh,
                   "usage: motor [status]\r\n"
                   "       motor start | stop\r\n"
                   "       motor freq [<hz>|+|-]   (0.5~10 Hz; no arg = query)\r\n"
                   "       motor mod  [<pct>|+|-]  (1~10 %%; no arg = query)\r\n"
                   "       motor iq [<A>]          (FOC torque ref; no arg = query)\r\n");
        return 0;
    }

    if (strcmp(sub, "start") == 0) {
        if (!app_terminal_cmd_require_no_fault(csh)) {
            return -1;
        }
        if (app_foc_is_active()) {
            csh_printf(csh, "ERR: FOC active (use 'foc off' first)\r\n");
            return -1;
        }
        if (app_debug_motor_is_running()) {
            csh_printf(csh, "motor already running\r\n");
            motor_print_status(csh);
            return 0;
        }
        app_terminal_cmd_capture_begin();
        app_debug_motor_rotation_toggle();
        app_terminal_cmd_capture_end();
        motor_print_status(csh);
        return 0;
    }

    if (strcmp(sub, "stop") == 0) {
        if (app_foc_is_active()) {
            csh_printf(csh, "ERR: FOC active (use 'foc off' first)\r\n");
            return -1;
        }
        app_terminal_cmd_capture_begin();
        app_debug_motor_stop();
        app_terminal_cmd_capture_end();
        motor_print_status(csh);
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

    if ((strcmp(sub, "freq") == 0) || (strcmp(sub, "mod") == 0)) {
        bool is_freq = (strcmp(sub, "freq") == 0);

        if (argc < 3) {
            motor_print_status(csh); /* 无参 = 查询 */
            return 0;
        }

        if ((strcmp(argv[2], "+") == 0) || (strcmp(argv[2], "-") == 0)) {
            int8_t dir = (argv[2][0] == '-') ? (int8_t)-1 : (int8_t)1;

            app_terminal_cmd_capture_begin();
            if (is_freq) {
                app_debug_motor_freq_step(dir);
            } else {
                app_debug_motor_mod_step(dir);
            }
            app_terminal_cmd_capture_end();
            motor_print_status(csh);
            return 0;
        }

        {
            float value;

            if (app_terminal_cmd_parse_float(argv[2], &value) != 0) {
                csh_printf(csh, "ERR: invalid value '%s'\r\n", argv[2]);
                return -1;
            }
            app_terminal_cmd_capture_begin();
            if (is_freq) {
                app_debug_motor_set_freq(value); /* 电频率 [Hz] */
            } else {
                app_debug_motor_set_mod(value / 100.0f); /* 调制比按百分比输入 */
            }
            app_terminal_cmd_capture_end();
            motor_print_status(csh);
            return 0;
        }
    }

    csh_printf(csh, "ERR: unknown subcommand '%s'\r\n", sub);
    csh_printf(csh,
               "usage: motor [status] | start | stop | freq [<hz>|+|-] | mod [<pct>|+|-] | "
               "iq [<A>]\r\n");
    return -1;
}

/**
 * @brief 命令 inv：三相逆变桥逐相输出控制（u/v/w/all/off）。
 */
static int cmd_inv(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);
    uint8_t mask;

    if (argc < 2) {
        return app_terminal_cmd_usage(csh, "inv <u|v|w|all|off>");
    }

    if (strcmp(argv[1], "u") == 0) {
        mask = 0x1U;
    } else if (strcmp(argv[1], "v") == 0) {
        mask = 0x2U;
    } else if (strcmp(argv[1], "w") == 0) {
        mask = 0x4U;
    } else if (strcmp(argv[1], "all") == 0) {
        mask = 0x7U;
    } else if (strcmp(argv[1], "off") == 0) {
        mask = 0x0U;
    } else {
        return app_terminal_cmd_usage(csh, "inv <u|v|w|all|off>");
    }

    /* 仅"使能输出"需要无故障；off 必须始终可用（安全优先） */
    if (mask != 0U) {
        if (!app_terminal_cmd_require_no_fault(csh)) {
            return -1;
        }
        if (app_foc_get_state() != APP_FOC_STATE_OFF) {
            csh_printf(csh, "ERR: FOC active (use 'foc off' first)\r\n");
            return -1;
        }
    }

    if (mask == 0U) {
        /* 紧急路径：先停 FOC（状态置 OFF），再关调试输出，避免 FOC 状态与桥状态失配 */
        app_foc_disable();
    }

    app_terminal_cmd_capture_begin();
    app_debug_motor_stop();
    app_debug_inverter_set_output(mask);
    app_terminal_cmd_capture_end();

    return 0;
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

/**
 * @brief 辨识失败原因名称
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
    case APP_IDENTIFY_REASON_NONFINITE: return "non-finite samples";
    case APP_IDENTIFY_REASON_VERIFY: return "verify failed";
    case APP_IDENTIFY_REASON_ENCODER: return "encoder error";
    case APP_IDENTIFY_REASON_FAULT: return "fault";
    case APP_IDENTIFY_REASON_STATE: return "state changed";
    default: return "?";
    }
}

/**
 * @brief cal encoder job：1kHz 推进辨识编排（进度/验证/结果打印）
 * @param now_ms 系统毫秒计数
 */
static void cal_encoder_tick(uint32_t now_ms) {
    app_motor_identify_result_t result;

    app_motor_identify_run_once(now_ms);
    if (app_motor_identify_is_active()) {
        return;
    }

    /* 结束：由 Comm 层读取 Control 结果并打印（分层：Comm → Control） */
    app_motor_identify_get_result(&result);
    if (result.done) {
        app_terminal_cmd_emit("\r\nOK: encoder identify  offset=%.4f rad (%.2f deg)  dir=%+.0f  "
                              "q=%.3f (tracking quality)\r\n",
                              (double)result.offset_rad,
                              (double)(result.offset_rad * (180.0f / FOC_PI_F)),
                              (double)result.direction, (double)result.quality);
        app_terminal_cmd_emit("    verify: mean=%.2f deg  max=%.2f deg (limit 5/15)\r\n",
                              (double)result.verify_mean_deg, (double)result.verify_max_deg);
        {
            const app_motor_params_t* motor = app_motor_params_current();
            float implied_pp = (float)motor->pole_pairs / (1.0f + result.ratio_err);

            app_terminal_cmd_emit(
                "    ratio_err=%+.1f%% (mech travel vs 2pi/p; RAM only, flash v2)\r\n",
                (double)(result.ratio_err * 100.0f));
            app_terminal_cmd_emit("    implied effective pole_pairs = %.1f (if encoder ratio is 1:1)\r\n",
                                  (double)implied_pp);
        }
    } else {
        app_terminal_cmd_emit("\r\nFAIL: encoder identify (%s)  q=%.3f  ratio_err=%+.1f%%\r\n",
                              cal_encoder_fail_name(result.fail_reason),
                              (double)result.quality, (double)(result.ratio_err * 100.0f));
        app_terminal_cmd_emit("      verify: mean=%.2f deg  max=%.2f deg (limit 15/45)\r\n",
                              (double)result.verify_mean_deg, (double)result.verify_max_deg);
        app_terminal_cmd_emit("      check: free rotation / I_cal enough / encoder mounting / "
                              "pole_pairs\r\n");
    }
    app_terminal_job_abort(); /* 结束 job（触发 abort 回调换行+刷新） */
}

/**
 * @brief cal encoder job 中止回调
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
 * @brief 命令 cal：电流零点标定（current）/ 电角度辨识（encoder），job 驱动，不阻塞控制环。
 */
static int cmd_cal(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);

    if (argc < 2) {
        return app_terminal_cmd_usage(csh, "cal current | cal encoder");
    }

    if (strcmp(argv[1], "encoder") == 0) {
        if (!app_terminal_cmd_require_no_fault(csh)) {
            return -1;
        }
        if (app_foc_get_state() == APP_FOC_STATE_OFF) {
            csh_printf(csh, "ERR: FOC not enabled (use 'foc on' first)\r\n");
            return -1;
        }
        if ((app_foc_get_state() != APP_FOC_STATE_READY)
            && (app_foc_get_state() != APP_FOC_STATE_RUN)) {
            csh_printf(csh, "ERR: FOC busy/FAULT (require READY or RUN, no fault)\r\n");
            return -1;
        }
        /* RUN 进入：先清零转矩给定（enter_calib 内部亦清零），避免切换瞬态 */
        if (app_debug_motor_is_running()) {
            csh_printf(csh, "ERR: V/F rotation running (stop first)\r\n");
            return -1;
        }
        csh_printf(csh,
                   "WARN: motor must be FREE to rotate; I_cal~2A; ~3.2s; any key aborts\r\n");
        /* 先启动 job（会中止旧 job），再启动辨识 */
        if (app_terminal_job_start(&s_cal_encoder_job) != 0) {
            csh_printf(csh, "FAIL: job start\r\n");
            return -1;
        }
        if (app_motor_identify_start() != 0) {
            app_terminal_job_abort();
            csh_printf(csh, "FAIL: identify start (fault/foc state/params)\r\n");
            return -1;
        }
        return 0;
    }

    if (strcmp(argv[1], "current") != 0) {
        return app_terminal_cmd_usage(csh, "cal current | cal encoder");
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

    /* 先启动 job（内部会中止旧 job → 旧 cal 的 abort 回调取消旧标定），再启动新标定，
     * 避免旧 job 的 cancel 误杀新标定 */
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
CSH_CMD_EXPORT_ALIAS(cmd_inv, inv, );
CSH_CMD_EXPORT_ALIAS(cmd_cal, cal, );
