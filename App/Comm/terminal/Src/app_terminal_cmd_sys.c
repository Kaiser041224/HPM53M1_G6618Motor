/**
 * @file    app_terminal_cmd_sys.c
 * @brief   Terminal 系统命令（ver / status / usb / reboot / driver / dfu）
 * @author  Kaiser
 *
 * 命令：
 *   ver    固件版本、构建时间、复位源状态
 *   status 单次系统快照（故障 / 电流 / 母线 / NTC / 编码器 / 主循环迟到）
 *   usb    USB 枚举与 Terminal 输出统计
 *   reboot 软件复位（需 `confirm` 二次确认 + 电机停止联锁）
 *   driver +12V 栅极供电手动开关（FOC OFF）
 *   dfu    查看 active/backup 槽状态；`dfu confirm` 写启动邮箱并复位进 DFU
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_terminal_cmd.h"

#include "app_3phase_inverter.h"
#include "app_analog_signal.h"
#include "app_boot.h"
#include "app_debug_encoder.h"
#include "app_encoder.h"
#include "app_fault.h"
#include "app_foc.h"
#include "app_terminal.h"
#include "app_usb.h"
#include "intf_sys.h"

#include <string.h>

/** 固件版本串 */
#define APP_TERMINAL_FW_VERSION "0.1.1"

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
 * @brief 命令 usb：链路统计（DTR / Terminal 就绪 / 丢弃 / TX 卡死恢复 / 总线复位）。
 */
static int cmd_usb(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);
    intf_usb_cdc_stats_t st = {0};

    app_usb_get_stats(&st);
    csh_printf(csh, "DTR=%u terminal ready=%u tx drop bytes=%u\r\n", app_usb_is_dtr() ? 1U : 0U,
               app_terminal_is_ready() ? 1U : 0U,
               (unsigned)app_terminal_get_tx_drop_bytes());
    csh_printf(csh, "cdc: tx_timeouts=%u (端点卡死强制恢复，应保持 0)  tx_drops=%u\r\n",
               (unsigned)st.tx_timeouts, (unsigned)st.tx_drops);
    csh_printf(csh, "     rx_drops=%u byte  bus_resets=%u  xfer_errors=%u\r\n",
               (unsigned)st.rx_drops, (unsigned)st.bus_resets, (unsigned)st.xfer_errors);
    {
        intf_usb_bulk_stats_t bs = {0};

        app_usb_bulk_get_stats(&bs);
        csh_printf(csh, "bulk: ready=%u rx_pkts=%u tx_pkts=%u\r\n",
                   app_usb_bulk_ready() ? 1U : 0U, (unsigned)bs.rx_packets,
                   (unsigned)bs.tx_packets);
        csh_printf(csh, "      rx_drops=%u byte  tx_drops=%u byte  oversize=%u  bus_resets=%u\r\n",
                   (unsigned)bs.rx_drops, (unsigned)bs.tx_drops, (unsigned)bs.oversize,
                   (unsigned)bs.bus_resets);
    }

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

/**
 * @brief 命令 driver power：FOC OFF 下手动开关 +12V 栅极供电
 */
static int cmd_driver(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);
    const char* what = (argc >= 2) ? argv[1] : NULL;

    if ((what == NULL) || (strcmp(what, "power") != 0)) {
        return app_terminal_cmd_usage(csh, "driver power [on|off]");
    }
    if ((argc < 3) || (strcmp(argv[2], "on") == 0) || (strcmp(argv[2], "off") == 0)) {
        if (argc < 3) {
            csh_printf(csh, "driver gate power: %s\r\n",
                       app_3phase_inverter_gate_power_is_on() ? "ON" : "OFF");
            return 0;
        }
    } else {
        return app_terminal_cmd_usage(csh, "driver power [on|off]");
    }

    if (app_foc_stop_pending()) {
        csh_printf(csh, "ERR: FOC stop pending; wait for state=OFF\r\n");
        return -1;
    }
    if (app_foc_get_state() != APP_FOC_STATE_OFF) {
        csh_printf(csh, "ERR: driver power requires FOC OFF (use 'foc off' first)\r\n");
        return -1;
    }

    if (strcmp(argv[2], "on") == 0) {
        if (app_3phase_inverter_gate_power_on() != 0) {
            csh_printf(csh, "ERR: gate power on rejected\r\n");
            return -1;
        }
        csh_printf(csh, "driver gate power: ON (PWM still disabled)\r\n");
    } else {
        if (app_3phase_inverter_gate_power_off() != 0) {
            csh_printf(csh, "ERR: gate power off rejected\r\n");
            return -1;
        }
        csh_printf(csh, "driver gate power: OFF\r\n");
    }
    return 0;
}

/**
 * @brief 命令 dfu：查看 active/backup 槽状态；`dfu confirm` 写启动邮箱并复位进 DFU
 *
 * 联锁：FOC 必须 OFF（桥已断开）+ 无停机挂起。进入 DFU 后设备重新枚举为
 * 0x34B7:0x6619，用 dfu-util 下载非活动槽镜像。
 */

/** @brief 槽号 → 大写显示名（0=ACTIVE / 1=BACKUP） */
static const char* dfu_slot_name(uint8_t slot) {
    static const char* const names[] = {"ACTIVE", "BACKUP"};

    return (slot < 2U) ? names[slot] : "NONE";
}

/** @brief 槽号 → 小写文件名（slot_<name>.bin） */
static const char* dfu_slot_file(uint8_t slot) {
    return (slot == 0U) ? "active" : "backup";
}

static int cmd_dfu(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);
    app_boot_status_t st;

    if ((argc >= 2) && (strcmp(argv[1], "confirm") == 0)) {
        if (!app_terminal_cmd_require_motor_stopped(csh)) {
            return -1;
        }
        if (app_foc_stop_pending()) {
            csh_printf(csh, "ERR: FOC stop pending; wait for state=OFF\r\n");
            return -1;
        }
        if (app_foc_get_state() != APP_FOC_STATE_OFF) {
            csh_printf(csh, "ERR: DFU requires FOC OFF ('foc off' first)\r\n");
            return -1;
        }
        if (app_boot_get_status(&st) != 0) {
            csh_printf(csh, "ERR: boot metadata unavailable\r\n");
            return -1;
        }
        csh_printf(csh, "entering DFU: target=slot %s, reset...\r\n",
                   dfu_slot_name(st.upgrade_target_slot));
        app_boot_request_dfu();
    }

    if ((argc >= 3) && (strcmp(argv[1], "slot") == 0)) {
        uint8_t slot;

        if ((strcmp(argv[2], "active") == 0) || (strcmp(argv[2], "a") == 0)
            || (strcmp(argv[2], "A") == 0)) {
            slot = 0U;
        } else if ((strcmp(argv[2], "backup") == 0) || (strcmp(argv[2], "b") == 0)
                   || (strcmp(argv[2], "B") == 0)) {
            slot = 1U;
        } else {
            return app_terminal_cmd_usage(csh, "dfu slot active|backup");
        }
        if (!app_terminal_cmd_require_motor_stopped(csh)) {
            return -1;
        }
        if (app_foc_stop_pending() || (app_foc_get_state() != APP_FOC_STATE_OFF)) {
            csh_printf(csh, "ERR: dfu slot requires FOC OFF\r\n");
            return -1;
        }
        if (app_boot_set_active(slot) != 0) {
            csh_printf(csh, "ERR: slot selection failed (legacy build or flash error)\r\n");
            return -1;
        }
        csh_printf(csh, "boot: boot slot set to %s; reboot to apply\r\n", dfu_slot_name(slot));
        return 0;
    }

    if ((argc >= 2) && (strcmp(argv[1], "show") != 0)) {
        return app_terminal_cmd_usage(csh, "dfu [show|confirm|slot active|backup]");
    }

    if (!app_boot_is_slot_build()) {
        csh_printf(csh, "boot: legacy image (no active/backup slot; flash-ota not installed)\r\n");
        return 0;
    }
    if (app_boot_get_status(&st) != 0) {
        csh_printf(csh, "ERR: boot metadata unavailable\r\n");
        return -1;
    }

    csh_printf(csh, "boot: running=%s boot=%s pending=%s attempts=%u confirmed=%u\r\n",
               dfu_slot_name(st.running_slot), dfu_slot_name(st.active_slot),
               dfu_slot_name(st.pending_slot), (unsigned)st.attempts,
               (unsigned)st.confirmed);
    {
        /* 自检要读整个槽 payload（约 25–30ms），只在 FOC OFF 时执行 */
        bool can_verify = (app_foc_get_state() == APP_FOC_STATE_OFF) && !app_foc_stop_pending();
        int self = can_verify ? app_boot_self_verify() : -3;
        const char *self_text = (self == 0)    ? "OK"
                                : (self == -2) ? "BAD(crc)"
                                : (self == -1) ? "n/a"
                                               : "skip(FOC on)";

        csh_printf(csh, "boot: image version=%u crc=0x%08X self=%s\r\n",
                   (unsigned)app_boot_image_version(), (unsigned)app_boot_image_crc(), self_text);
    }
    csh_printf(csh, "boot: DFU target=%s (%s)\r\n", dfu_slot_name(st.upgrade_target_slot),
               st.upgrade_target_valid ? "valid image present" : "empty");
    csh_printf(csh, "usage: dfu confirm  (FOC OFF; then dfu-util -a 0 -D slot_%s.bin)\r\n",
               dfu_slot_file(st.upgrade_target_slot));
    csh_printf(csh, "       dfu slot active|backup  (force next boot slot)\r\n");
    return 0;
}

CSH_CMD_EXPORT_ALIAS(cmd_ver, ver, );
CSH_CMD_EXPORT_ALIAS(cmd_status, status, );
CSH_CMD_EXPORT_ALIAS(cmd_usb, usb, );
CSH_CMD_EXPORT_ALIAS(cmd_reboot, reboot, );
CSH_CMD_EXPORT_ALIAS(cmd_driver, driver, );
CSH_CMD_EXPORT_ALIAS(cmd_dfu, dfu, );
