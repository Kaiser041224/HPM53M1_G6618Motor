/**
 * @file    app_terminal_cmd_diag.c
 * @brief   Terminal 诊断命令（adc / pwm / enc / fault / profiler）
 * @author  Kaiser
 *
 * 命令：
 *   adc dump | diag | delay <ns>
 *   pwm dump
 *   enc info | zero <rotor|output> | clear
 *   fault show | clear
 *   profiler dump
 *
 * 说明：复用 Debug 层既有 dump 函数（单向依赖），经 capture 旁路送入终端；
 *       命令本体必须有界、快速返回（运行在 1kHz 慢任务）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_terminal_cmd.h"

#include "app_adc.h"
#include "app_can.h"
#include "app_can_service.h"
#include "app_debug_adc.h"
#include "app_debug_encoder.h"
#include "app_debug_fault.h"
#include "app_debug_hrpwm.h"
#include "app_debug_profiler.h"
#include "app_encoder.h"
#include "intf_clock.h"

#include <string.h>

/**
 * @brief 打印两路编码器的角度（十进制浮点）、计数与零点。
 * @param csh terminal 实例
 */
static void print_encoder_info(chry_shell_t* csh) {
    static const char* const names[APP_ENCODER_COUNT] = {"rotor", "output"};

    for (uint8_t i = 0U; i < (uint8_t)APP_ENCODER_COUNT; i++) {
        app_encoder_id_t id = (app_encoder_id_t)i;
        float deg = 0.0f;
        uint16_t pos = 0U;
        uint16_t zero = 0U;

        if (app_encoder_read_deg(id, &deg) != 0) {
            csh_printf(csh, "enc: %-6s no data\r\n", names[i]);
            continue;
        }
        (void)app_encoder_read_position(id, &pos);
        (void)app_encoder_get_zero(id, &zero);
        csh_printf(csh, "enc: %-6s pos=%9.2f deg  (counts=%5u, zero=%5u)\r\n", names[i],
                   (double)deg, (unsigned)pos, (unsigned)zero);
    }
    csh_printf(csh, "     param=%s\r\n", app_encoder_is_param_loaded() ? "valid" : "default");
}

/**
 * @brief 命令 adc：dump / diag / delay <ns>。
 */
static int cmd_adc(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);

    if (argc < 2) {
        return app_terminal_cmd_usage(csh, "adc dump|diag|delay <ns>");
    }

    if (strcmp(argv[1], "dump") == 0) {
        app_terminal_cmd_run_dump(app_debug_adc_dump_channels);
    } else if (strcmp(argv[1], "diag") == 0) {
        app_terminal_cmd_run_dump(app_debug_adc_dump_diag);
    } else if (strcmp(argv[1], "delay") == 0) {
        uint32_t delay_ns;

        if ((argc < 3) || (app_terminal_cmd_parse_u32(argv[2], &delay_ns) != 0)) {
            return app_terminal_cmd_usage(csh, "adc delay <ns>");
        }
        if (app_adc_set_trigger_delay_ns(delay_ns) == 0) {
            csh_printf(csh, "OK: ADC trigger delay = %u ns\r\n", (unsigned)delay_ns);
        } else {
            csh_printf(csh, "FAIL: ADC trigger delay set\r\n");
        }
    } else {
        return app_terminal_cmd_usage(csh, "adc dump|diag|delay <ns>");
    }

    return 0;
}

/**
 * @brief 命令 pwm：dump（compare 快照 + 频率配置）。
 */
static int cmd_pwm(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);

    if ((argc < 2) || (strcmp(argv[1], "dump") != 0)) {
        return app_terminal_cmd_usage(csh, "pwm dump");
    }

    app_terminal_cmd_run_dump(app_debug_dump_hrpwm_cmp);
    app_terminal_cmd_run_dump(app_debug_dump_hrpwm_freq);

    return 0;
}

/**
 * @brief 命令 enc：info / zero <rotor|output> / clear。
 */
static int cmd_enc(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);

    if (argc < 2) {
        return app_terminal_cmd_usage(csh, "enc info|zero <rotor|output>|clear");
    }

    if (strcmp(argv[1], "info") == 0) {
        print_encoder_info(csh);
        return 0;
    }

    if (strcmp(argv[1], "zero") == 0) {
        app_encoder_id_t id;

        if (argc < 3) {
            return app_terminal_cmd_usage(csh, "enc zero <rotor|output>");
        }
        if (strcmp(argv[2], "rotor") == 0) {
            id = APP_ENCODER_ROTOR;
        } else if (strcmp(argv[2], "output") == 0) {
            id = APP_ENCODER_OUTPUT;
        } else {
            return app_terminal_cmd_usage(csh, "enc zero <rotor|output>");
        }
        if (!app_terminal_cmd_require_motor_stopped(csh)) {
            return -1;
        }
        csh_printf(csh, "writing flash (sector erase, expect ~20 ms stall)...\r\n");
        if (app_encoder_set_zero(id) == 0) {
            uint16_t zero = 0U;

            (void)app_encoder_get_zero(id, &zero);
            csh_printf(csh, "OK: %s zero set, new zero=%u (counts)\r\n",
                       (id == APP_ENCODER_ROTOR) ? "rotor" : "output", (unsigned)zero);
        } else {
            csh_printf(csh, "FAIL: zero set\r\n");
        }
        return 0;
    }

    if (strcmp(argv[1], "clear") == 0) {
        if (!app_terminal_cmd_require_motor_stopped(csh)) {
            return -1;
        }
        csh_printf(csh, "writing flash (sector erase, expect ~20 ms stall)...\r\n");
        (void)app_encoder_clear_zero(APP_ENCODER_ROTOR);
        (void)app_encoder_clear_zero(APP_ENCODER_OUTPUT);
        csh_printf(csh, "OK: zero cleared (rotor + output)\r\n");
        return 0;
    }

    return app_terminal_cmd_usage(csh, "enc info|zero <rotor|output>|clear");
}

/**
 * @brief 命令 fault：show / clear。
 */
static int cmd_fault(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);

    if (argc < 2) {
        return app_terminal_cmd_usage(
            csh,
            "fault show|clear|protect [on|off]|policy [show|<code> <warn|derate|stop|trip>]|"
            "inject <code> on|off");
    }

    if (strcmp(argv[1], "show") == 0) {
        app_terminal_cmd_run_dump(app_debug_fault_dump);
    } else if (strcmp(argv[1], "clear") == 0) {
        app_terminal_cmd_capture_begin();
        app_debug_fault_clear();
        app_terminal_cmd_capture_end();
    } else if ((strcmp(argv[1], "policy") == 0) || (strcmp(argv[1], "inject") == 0)
               || (strcmp(argv[1], "protect") == 0)) {
        return app_terminal_cmd_fault_action_cmd(csh, argc, argv);
    } else {
        return app_terminal_cmd_usage(csh,
                                      "fault show|clear|protect|policy|inject");
    }

    return 0;
}

/**
 * @brief 命令 profiler：dump（记录上次调用 cycle，计算窗口周期数）。
 */
static int cmd_profiler(int argc, char** argv) {
    static uint32_t s_last_profiler_cycle;
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);
    uint32_t cpu_freq;
    uint32_t now;
    uint32_t elapsed;

    if ((argc < 2) || (strcmp(argv[1], "dump") != 0)) {
        return app_terminal_cmd_usage(csh, "profiler dump");
    }

    cpu_freq = intf_clock_get_cpu_freq();
    now = intf_clock_get_cycle();
    elapsed = (s_last_profiler_cycle == 0U) ? cpu_freq : (now - s_last_profiler_cycle);
    s_last_profiler_cycle = now;

    app_terminal_cmd_capture_begin();
    app_debug_profiler_dump(cpu_freq, elapsed);
    app_terminal_cmd_capture_end();

    return 0;
}

/**
 * @brief 命令 can：CAN 协议与看门狗状态
 *
 * 输出：节点号、看门狗（armed/suspended/fired）、总线统计、协议统计。
 * 台架排查顺序：先看 wd armed（主机是否发过控制帧）→ 再看 proto.drop/bad（帧是否被丢）
 * → 最后看 bus_off/tx 失败（物理层）。
 */
static int cmd_can(chry_shell_t* csh, int argc, const char** argv) {
    (void)argc;
    (void)argv;
    app_can_service_stats_t proto;
    app_can_stats_t bus;
    intf_can_status_t st;
    bool armed = false;
    bool suspended = false;
    bool fired = false;

    app_can_service_get_stats(&proto);
    (void)app_can_get_stats(&bus);
    (void)app_can_get_status(&st);
    app_can_service_get_watchdog(&armed, &suspended, &fired);

    csh_printf(csh, "can: node=%u wd=%s susp=%u fired=%u | bus_off=%d\r\n",
               (unsigned)app_can_service_get_node_id(), armed ? "armed" : "idle",
               (unsigned)suspended, (unsigned)fired, st.bus_off);
    csh_printf(csh, "     bus: tx_ok=%u tx_fail=%u rx=%u drop=%u\r\n",
               (unsigned)bus.tx_ok_count, (unsigned)bus.tx_fail_count, (unsigned)bus.rx_count,
               (unsigned)bus.rx_drop_count);
    csh_printf(csh, "     proto: rx=%u drop=%u bad=%u unk=%u fmt=%u mit(ok=%u to=%u)\r\n",
               (unsigned)proto.rx_frames, (unsigned)proto.rx_dropped, (unsigned)proto.bad_len,
               (unsigned)proto.unknown_cmd, (unsigned)proto.format_mismatch,
               (unsigned)proto.mit_pair_ok, (unsigned)proto.mit_pair_timeout);
    return 0;
}

CSH_CMD_EXPORT_ALIAS(cmd_can, can, );
CSH_CMD_EXPORT_ALIAS(cmd_adc, adc, );
CSH_CMD_EXPORT_ALIAS(cmd_pwm, pwm, );
CSH_CMD_EXPORT_ALIAS(cmd_enc, enc, );
CSH_CMD_EXPORT_ALIAS(cmd_fault, fault, );
CSH_CMD_EXPORT_ALIAS(cmd_profiler, profiler, );
