/**
 * @file    app_terminal_cmd_motor.c
 * @brief   Terminal 电机命令（motor / inv / cal）
 * @author  Kaiser
 *
 * 命令：
 *   motor start | stop | freq <+|-> | mod <+|->   （开环 V/F 自检）
 *   motor iq [<A>]                               （FOC 转矩给定/查询）
 *   motor iq [<A>]                               （FOC 转矩给定；无参 = 查询）
 *   inv <u|v|w|all|off>                          （三相逆变桥逐相输出）
 *   cal current                                  （电流零点标定）
 *
 * 安全联锁（设计文档 §8）：
 *   驱动类命令要求无故障锁存；先停旋转；标定要求电机停止。
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
        csh_printf(csh, "foc iq: ref=%.3f A  meas=%.3f A\r\n", (double)app_foc_get_iq_ref(),
                   (double)snap.i_q_a);
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
 * @brief 命令 cal：电流零点标定（current，job 驱动，不阻塞控制环）。
 */
static int cmd_cal(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);

    if ((argc < 2) || (strcmp(argv[1], "current") != 0)) {
        return app_terminal_cmd_usage(csh, "cal current");
    }
    if (!app_terminal_cmd_require_motor_stopped(csh)) {
        return -1;
    }
    if (!app_terminal_cmd_require_no_fault(csh)) {
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
