/**
 * @file    app_terminal_cmd_motor.c
 * @brief   Terminal 电机命令（motor / cal）—— M1：仅 FOC 转矩给定与电流零点标定
 * @author  Kaiser
 *
 * 命令：
 *   motor iq [<A>]     （FOC 转矩给定；无参 = 查询）
 *   cal current        （电流零点标定）
 *
 * M1 裁剪（spec §1/§4）：V/F 开环（motor start/stop/freq/mod）、逆变桥逐相
 * （inv）、电角度辨识（cal encoder）不接入；相关源文件不参与构建。重新接入时
 * 恢复本文件相应分支，并把 app_debug_motor.c / app_motor_identify.c 加回
 * CMakeLists.txt。
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

/**
 * @brief 命令 cal：电流零点标定（current），job 驱动，不阻塞控制环。
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
