/**
 * @file    app_terminal_cmd_sys.c
 * @brief   Terminal 系统命令（ver / status / usb / reboot）
 * @author  Kaiser
 *
 * 命令：
 *   ver    固件版本、构建时间、复位源状态
 *   status 单次系统快照（故障 / 电流 / 母线 / NTC / 编码器 / 主循环迟到）
 *   usb    USB 枚举与 Terminal 输出统计
 *   reboot 软件复位（需 `confirm` 二次确认 + 电机停止联锁）
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_terminal_cmd.h"

#include "app_analog_signal.h"
#include "app_debug_encoder.h"
#include "app_encoder.h"
#include "app_fault.h"
#include "app_terminal.h"
#include "app_usb.h"
#include "intf_sys.h"

#include <string.h>

/** 固件版本串 */
#define APP_TERMINAL_FW_VERSION "0.1.0"

/**
 * @brief 命令 ver：打印固件版本、构建时间与复位源状态。
 */
static int cmd_ver(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);

    csh_printf(
        csh, "HPM53M1_G6618Motor fw v%s (build %s %s)\r\n", APP_TERMINAL_FW_VERSION, __DATE__,
        __TIME__);
    csh_printf(csh, "reset status: 0x%08X\r\n", (unsigned)intf_sys_get_reset_status());

    return 0;
}

/**
 * @brief 命令 clear：清屏并重绘提示符。
 */
static int cmd_clear(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);

    (void)csh;

    app_terminal_cmd_emit("\033[2J\033[H");
    app_terminal_refresh();

    return 0;
}
CSH_CMD_EXPORT_ALIAS(cmd_clear, clear, );

/**
 * @brief 命令 status：单次系统快照（故障 / 模拟量 / 编码器 / 主循环迟到）。
 */
static int cmd_status(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);
    app_analog_values_t values;
    float deg_rotor = 0.0f;
    float deg_output = 0.0f;
    uint32_t state = (uint32_t)app_fault_get_state();

    {
        char codes_text[80];
        char latched_text[80];

        app_terminal_cmd_fault_codes_text(app_fault_get_codes(), codes_text, sizeof(codes_text));
        app_terminal_cmd_fault_codes_text(app_fault_get_latched(), latched_text,
                                       sizeof(latched_text));
        csh_printf(csh,
                   "fault   state=%-7s codes=0x%08X (%s)\r\n"
                   "        latched=0x%08X (%s) first=0x%08X\r\n",
                   app_terminal_cmd_fault_state_name((uint8_t)state),
                   (unsigned)app_fault_get_codes(), codes_text,
                   (unsigned)app_fault_get_latched(), latched_text,
                   (unsigned)app_fault_get_first());
    }

    if (app_analog_signal_read_all(&values)) {
        csh_printf(
            csh,
            "analog  i_u=%7.3fA i_v=%7.3fA i_w=%7.3fA v_bus=%7.2fV ntc0=%8.1fohm "
            "ntc1=%8.1fohm\r\n",
            (double)values.i_u_a, (double)values.i_v_a, (double)values.i_w_a,
            (double)values.v_bus_v, (double)values.r_ntc0_ohm, (double)values.r_ntc1_ohm);
    } else {
        csh_printf(
            csh, "analog  i_u=    n/a i_v=    n/a i_w=    n/a v_bus=    n/a ntc0=     n/a "
                 "ntc1=     n/a\r\n");
    }

    (void)app_encoder_read_deg(APP_ENCODER_ROTOR, &deg_rotor);
    (void)app_encoder_read_deg(APP_ENCODER_OUTPUT, &deg_output);
    csh_printf(csh,
               "encoder rotor=%8.2f deg output=%8.2f deg param=%s loop_late=%uus\r\n",
               (double)deg_rotor, (double)deg_output,
               app_encoder_is_param_loaded() ? "valid" : "default",
               (unsigned)g_enc_loop_late_us);

    return 0;
}

/**
 * @brief 命令 usb：DTR / Terminal 就绪 / TX 丢弃字节统计。
 */
static int cmd_usb(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);

    csh_printf(
        csh, "DTR=%u terminal ready=%u tx drop bytes=%u\r\n", app_usb_is_dtr() ? 1U : 0U,
        app_terminal_is_ready() ? 1U : 0U, (unsigned)app_terminal_get_tx_drop_bytes());

    return 0;
}

/**
 * @brief 命令 reboot：二次确认 + 电机停止联锁后软件复位。
 */
static int cmd_reboot(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);

    if ((argc < 2) || (strcmp(argv[1], "confirm") != 0)) {
        return app_terminal_cmd_usage(csh, "reboot confirm");
    }
    if (!app_terminal_cmd_require_motor_stopped(csh)) {
        return -1;
    }

    csh_printf(csh, "rebooting...\r\n");
    intf_sys_reset();

    return 0;
}

CSH_CMD_EXPORT_ALIAS(cmd_ver, ver, );
CSH_CMD_EXPORT_ALIAS(cmd_status, status, );
CSH_CMD_EXPORT_ALIAS(cmd_usb, usb, );
CSH_CMD_EXPORT_ALIAS(cmd_reboot, reboot, );
