/**
 * @file    app_terminal_monitor.c
 * @brief   Terminal monitor 常驻状态区（1kHz 驱动，2Hz 刷新；REPL 可同时使用）
 * @author  Kaiser
 *
 * 形态：列表式状态区（一值一行，标签 + 管道符 `|` + 颜色），位于提示符**上方**；
 *       显示期间可正常输入/执行命令（非 job，不吞输入）。
 *
 * 绘制原理（仅依赖相对光标移动，无需终端位置查询）：
 *   \033[J      擦除光标下方（旧状态区 + 残留）
 *   \r\n        换行到状态区首行
 *   逐行打印    （每行以 \r\n 结束）
 *   \033[<n>A   上移回提示符行
 *   refresh     重绘提示符 + 当前输入（光标回到行编辑位置）
 *
 * 命令：`monitor`（切换）/ `monitor on|off`。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_terminal_monitor.h"

#include "app_terminal.h"
#include "app_terminal_cmd.h"

#include "app_analog_signal.h"
#include "app_debug_encoder.h"
#include "app_encoder.h"
#include "app_fault.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define APP_MONITOR_PERIOD_MS (500U) /**< 刷新周期（2 Hz） */
#define APP_MONITOR_LINES     (12U)  /**< 状态区行数（与绘制行数一致） */

#define APP_MONITOR_LABEL "\033[36m" /**< 标签颜色（青） */
#define APP_MONITOR_RESET "\033[0m"  /**< 颜色复位 */

static bool s_monitor_active;      /**< 状态区启用 */
static bool s_monitor_force;       /**< 强制首帧 */
static bool s_block_drawn;         /**< 状态区已绘制（决定首帧走插入路径） */
static uint32_t s_draw_line_feeds; /**< 上次绘制时的换行计数快照 */
static uint32_t s_monitor_last_ms; /**< 上次刷新毫秒 */

bool app_terminal_monitor_is_active(void) {
    return s_monitor_active;
}

void app_terminal_monitor_set(bool on) {
    if (s_monitor_active == on) {
        return;
    }

    s_monitor_active = on;
    if (on) {
        s_monitor_force = true;
    } else if (s_block_drawn) {
        /* 删除状态区（上移到首行 → 删除 N 行 → 提示符上移 → 重绘） */
        app_terminal_cmd_emit("\033[%uA\r\033[%uM", (unsigned)APP_MONITOR_LINES,
                           (unsigned)APP_MONITOR_LINES);
        s_block_drawn = false;
        app_terminal_refresh();
    } else {
        /* 无状态区：无需清理 */
    }
}

/**
 * @brief 绘制一行（标签 | 值）。
 * @param label 标签（≤6 字符）
 * @param format 值格式串
 * @param ... 可变参数
 */
static void monitor_line(const char* label, const char* format, ...) {
    char value[64];
    va_list ap;
    int length;

    va_start(ap, format);
    length = vsnprintf(value, sizeof(value), format, ap);
    va_end(ap);

    if (length > 0) {
        if (length > (int)sizeof(value) - 1) {
            length = (int)sizeof(value) - 1;
        }
        app_terminal_cmd_emit(" " APP_MONITOR_LABEL "%-6s" APP_MONITOR_RESET "| %s\r\n", label, value);
    }
}

/**
 * @brief 绘制故障行（整行底色高亮）。
 */
static void monitor_fault_line(void) {
    uint32_t state = (uint32_t)app_fault_get_state();
    uint32_t codes = app_fault_get_codes();
    uint32_t latched = app_fault_get_latched();
    char codes_text[64];
    char latched_text[64];

    app_terminal_cmd_fault_codes_text(codes, codes_text, sizeof(codes_text));
    app_terminal_cmd_fault_codes_text(latched, latched_text, sizeof(latched_text));

    if ((state == (uint32_t)APP_FAULT_STATE_FAULT) || (latched != 0U)) {
        app_terminal_cmd_emit(" \033[1;41;97m%-6s" APP_MONITOR_RESET "| %s (latched)\r\n", "FAULT",
                           codes_text);
    } else if (state == (uint32_t)APP_FAULT_STATE_WARNING) {
        app_terminal_cmd_emit(" \033[1;43;30m%-6s" APP_MONITOR_RESET "| %s\r\n", "WARN", codes_text);
    } else {
        app_terminal_cmd_emit(" \033[1;42;30m%-6s" APP_MONITOR_RESET "| no fault\r\n", "OK");
    }

    if (latched != 0U) {
        app_terminal_cmd_emit(" " APP_MONITOR_LABEL "%-6s" APP_MONITOR_RESET "| \033[31m%s\033[0m\r\n",
                           "LATCH", latched_text);
    } else {
        app_terminal_cmd_emit(" " APP_MONITOR_LABEL "%-6s" APP_MONITOR_RESET "| none\r\n", "LATCH");
    }
}

/**
 * @brief 绘制状态区（列表 → 回到/移动到提示符行 → 重绘提示符）。
 *
 * 路径选择（CherryRL 的 edit_refresh 会擦除光标下方，故状态区画在提示符**上方**）：
 *   - 空闲（自上次绘制后无换行输出）：原位更新
 *     `\033[<N>A` 上移到状态区首行 → `\r\033[J` 擦除（含旧提示符行）→ 重绘 → refresh
 *   - 有输出/首帧：在提示符上方插入 N 行（原提示符行下移）
 *     `\r\033[<N>L` → 重绘 → refresh
 */
static void monitor_draw(void) {
    app_analog_values_t values;
    bool analog_valid = app_analog_signal_read_all(&values);
    float deg_rotor = 0.0f;
    float deg_output = 0.0f;
    bool rotor_ok = (app_encoder_read_deg(APP_ENCODER_ROTOR, &deg_rotor) == 0);
    bool output_ok = (app_encoder_read_deg(APP_ENCODER_OUTPUT, &deg_output) == 0);

    if (s_block_drawn && (app_terminal_get_line_feeds() == s_draw_line_feeds)) {
        /* 空闲：原位更新 */
        app_terminal_cmd_emit("\033[%uA\r\033[J", (unsigned)APP_MONITOR_LINES);
    } else {
        /* 有输出/首帧：插入状态区（提示符行下移） */
        app_terminal_cmd_emit("\r\033[%uL", (unsigned)APP_MONITOR_LINES);
    }

    monitor_fault_line(); /* 1-2 行：故障 + 锁存 */

    if (analog_valid) {
        monitor_line("I_U", "%9.3f A", (double)values.i_u_a);
        monitor_line("I_V", "%9.3f A", (double)values.i_v_a);
        monitor_line("I_W", "%9.3f A", (double)values.i_w_a);
        monitor_line("V_BUS", "%9.2f V", (double)values.v_bus_v);
        monitor_line("NTC0", "%10.1f ohm", (double)values.r_ntc0_ohm);
        monitor_line("NTC1", "%10.1f ohm", (double)values.r_ntc1_ohm);
    } else {
        monitor_line("I_U", "n/a");
        monitor_line("I_V", "n/a");
        monitor_line("I_W", "n/a");
        monitor_line("V_BUS", "n/a");
        monitor_line("NTC0", "n/a");
        monitor_line("NTC1", "n/a");
    }

    if (rotor_ok) {
        monitor_line("ENC_R", "%9.2f deg", (double)deg_rotor);
    } else {
        monitor_line("ENC_R", "n/a");
    }
    if (output_ok) {
        monitor_line("ENC_O", "%9.2f deg", (double)deg_output);
    } else {
        monitor_line("ENC_O", "n/a");
    }

    monitor_line("PARAM", "%s", app_encoder_is_param_loaded() ? "valid" : "default");
    if (g_enc_loop_late_us != 0U) {
        app_terminal_cmd_emit(" " APP_MONITOR_LABEL "%-6s" APP_MONITOR_RESET "| \033[33m%u us\033[0m\r\n",
                           "LATE", (unsigned)g_enc_loop_late_us);
    } else {
        monitor_line("LATE", "%u us", (unsigned)g_enc_loop_late_us);
    }

    /* 重绘提示符 + 当前输入（光标回到行编辑位置） */
    app_terminal_refresh();

    s_block_drawn = true;
    s_draw_line_feeds = app_terminal_get_line_feeds();
}

void app_terminal_monitor_run_once(uint32_t now_ms) {
    if (!s_monitor_active) {
        return;
    }

    if (!s_monitor_force && ((uint32_t)(now_ms - s_monitor_last_ms) < APP_MONITOR_PERIOD_MS)) {
        return;
    }

    s_monitor_force = false;
    s_monitor_last_ms = now_ms;
    monitor_draw();
}

/**
 * @brief 命令 monitor：切换常驻状态区。
 */
static int cmd_monitor(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);

    if (argc >= 2) {
        if (strcmp(argv[1], "on") == 0) {
            app_terminal_monitor_set(true);
            csh_printf(csh, "monitor: ON (Ctrl-C or 'monitor off' to stop)\r\n");
            return 0;
        }
        if (strcmp(argv[1], "off") == 0) {
            app_terminal_monitor_set(false);
            csh_printf(csh, "monitor: OFF\r\n");
            return 0;
        }
        return app_terminal_cmd_usage(csh, "monitor [on|off]");
    }

    app_terminal_monitor_set(!app_terminal_monitor_is_active());
    csh_printf(csh, "monitor: %s\r\n", app_terminal_monitor_is_active() ? "ON" : "OFF");
    return 0;
}
CSH_CMD_EXPORT_ALIAS(cmd_monitor, monitor, );
