/**
 * @file    app_terminal_cmd.c
 * @brief   Terminal 命令公共工具实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_terminal_cmd.h"

#include "app_debug_rtt.h"
#include "app_fault.h"
#include "app_foc.h"
#include "app_terminal.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int app_terminal_cmd_usage(chry_shell_t* csh, const char* usage) {
    csh_printf(csh, "usage: %s\r\n", (usage != NULL) ? usage : "");
    return -1;
}

int app_terminal_cmd_parse_u32(const char* text, uint32_t* out) {
    char* end = NULL;
    unsigned long value;

    if ((text == NULL) || (out == NULL) || (text[0] == '\0') || (text[0] == '-')) {
        return -1; /* 拒绝负号（避免 "-1" 经 strtoul 回绕为 0xFFFFFFFF） */
    }

    errno = 0;
    value = strtoul(text, &end, 0);
    if ((errno != 0) || (end == text) || (*end != '\0') || (value > 0xFFFFFFFFUL)) {
        return -1;
    }

    *out = (uint32_t)value;
    return 0;
}

int app_terminal_cmd_parse_i32(const char* text, int32_t* out) {
    char* end = NULL;
    long value;

    if ((text == NULL) || (out == NULL) || (text[0] == '\0')) {
        return -1;
    }

    errno = 0;
    value = strtol(text, &end, 0);
    if ((errno != 0) || (end == text) || (*end != '\0') || (value < -2147483648L)
        || (value > 2147483647L)) {
        return -1;
    }

    *out = (int32_t)value;
    return 0;
}

int app_terminal_cmd_parse_float(const char* text, float* out) {
    char* end = NULL;
    float value;

    if ((text == NULL) || (out == NULL) || (text[0] == '\0')) {
        return -1;
    }

    errno = 0;
    value = strtof(text, &end);
    if ((errno != 0) || (end == text) || (*end != '\0') || !isfinite(value)) {
        return -1; /* 拒绝 nan / inf（避免污染控制量） */
    }

    *out = value;
    return 0;
}

bool app_terminal_cmd_require_motor_stopped(chry_shell_t* csh) {
    /* V/F 开环已裁剪（M1）：无独立旋转源，恒真。
     * FOC 互斥由 app_foc_enable（FAULT 需先 foc off / 非 OFF 幂等返回）自检，
     * 不在此拦截——否则 FAULT 态的 foc on 恢复路径被误拒。 */
    (void)csh;
    return true;
}

bool app_terminal_cmd_require_no_fault(chry_shell_t* csh) {
    /* 只拦"带动作码"（policy ∈ {STOP,TRIP}）的锁存 —— 与 app_foc_enable() 的契约一致。
     * 此前用 get_latched() + actions_enabled：任一 WARN 码（如 ENC_ERR）锁存后也会拒绝
     * foc on / motion 指令，与"WARN 不阻塞使能"的设计相反（台架实测复现）。
     * 动作总开关关闭时生效表全为 WARN → 该掩码恒为 0 → 自然不拦截，无需额外判断。 */
    uint32_t blocking = app_fault_get_blocking_latched();

    if (blocking != 0U) {
        csh_printf(csh, "ERR: fault latched 0x%08X, clear it first (fault clear)\r\n",
                   (unsigned)blocking);
        return false;
    }
    return true;
}

/**
 * @brief dump 旁路写入器：Debug 输出 → 终端 TX 环。
 * @param text 文本
 * @param len 长度
 */
static void app_terminal_cmd_writer(const char* text, size_t len) {
    app_terminal_write(text, len);
}

void app_terminal_cmd_capture_begin(void) {
    app_debug_set_writer(app_terminal_cmd_writer);
}

void app_terminal_cmd_capture_end(void) {
    app_debug_set_writer(NULL);
}

void app_terminal_cmd_run_dump(void (*dump_fn)(void)) {
    if (dump_fn == NULL) {
        return;
    }

    app_terminal_cmd_capture_begin();
    dump_fn();
    app_terminal_cmd_capture_end();
}

const char* app_terminal_cmd_fault_state_name(uint8_t state) {
    static const char* const s_fault_state_names[] = {
        "INIT",
        "NORMAL",
        "WARNING",
        "FAULT",
    };

    return (state < (sizeof(s_fault_state_names) / sizeof(s_fault_state_names[0])))
               ? s_fault_state_names[state]
               : "?";
}

/** 故障位 → 名称（与 app_fault.h 位定义对应） */
typedef struct {
    uint32_t bit;      /**< 位掩码 */
    const char* name;  /**< 名称 */
} app_terminal_fault_bit_t;

static const app_terminal_fault_bit_t s_fault_bits[] = {
    {APP_FAULT_OC_FAST_U, "OC_FAST_U"}, {APP_FAULT_OC_FAST_V, "OC_FAST_V"},
    {APP_FAULT_OC_FAST_W, "OC_FAST_W"}, {APP_FAULT_OC_SLOW_U, "OC_SLOW_U"},
    {APP_FAULT_OC_SLOW_V, "OC_SLOW_V"}, {APP_FAULT_OC_SLOW_W, "OC_SLOW_W"},
    {APP_FAULT_VBUS_OV, "VBUS_OV"},     {APP_FAULT_VBUS_UV, "VBUS_UV"},
    {APP_FAULT_ADC_TIMEOUT, "ADC_TMO"}, {APP_FAULT_ENC_READ, "ENC_ERR"},
    {APP_FAULT_OC_TRIP, "OC_TRIP"},
};

void app_terminal_cmd_fault_codes_text(uint32_t codes, char* out, size_t out_size) {
    size_t used = 0U;

    if ((out == NULL) || (out_size == 0U)) {
        return;
    }
    out[0] = '\0';

    if (codes == APP_FAULT_NONE) {
        (void)snprintf(out, out_size, "none");
        return;
    }

    for (size_t i = 0U; i < sizeof(s_fault_bits) / sizeof(s_fault_bits[0]); i++) {
        int written;

        if ((codes & s_fault_bits[i].bit) == 0U) {
            continue;
        }
        written = snprintf(&out[used], out_size - used, "%s%s", (used > 0U) ? "|" : "",
                           s_fault_bits[i].name);
        if ((written < 0) || ((size_t)written >= (out_size - used))) {
            break; /* 缓冲不足：截断 */
        }
        used += (size_t)written;
    }
}

void app_terminal_cmd_emit(const char* format, ...) {
    char buffer[192];
    va_list ap;
    int length;

    if (format == NULL) {
        return;
    }

    va_start(ap, format);
    length = vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);

    if (length > 0) {
        if (length > (int)sizeof(buffer) - 1) {
            length = (int)sizeof(buffer) - 1;
        }
        app_terminal_write(buffer, (size_t)length);
    }
}

/* ============================================================================
 * fault policy / fault inject（2026-10-27 v2：逐故障动作策略 + 台架注入）
 * ============================================================================ */

typedef struct {
    const char* name;
    uint32_t code;
} app_terminal_fault_code_t;

static const app_terminal_fault_code_t s_fault_code_names[] = {
    {"OC_FAST_U", APP_FAULT_OC_FAST_U},   {"OC_FAST_V", APP_FAULT_OC_FAST_V},
    {"OC_FAST_W", APP_FAULT_OC_FAST_W},   {"OC_SLOW_U", APP_FAULT_OC_SLOW_U},
    {"OC_SLOW_V", APP_FAULT_OC_SLOW_V},   {"OC_SLOW_W", APP_FAULT_OC_SLOW_W},
    {"VBUS_OV", APP_FAULT_VBUS_OV},       {"VBUS_UV", APP_FAULT_VBUS_UV},
    {"ADC_TMO", APP_FAULT_ADC_TIMEOUT},   {"ENC_ERR", APP_FAULT_ENC_READ},
    {"OC_TRIP", APP_FAULT_OC_TRIP},
};

static const char* const s_action_names[] = {"warn", "derate", "stop", "trip"};

const char* app_terminal_cmd_fault_action_name(uint8_t act) {
    return (act <= (uint8_t)APP_FLT_ACT_TRIP) ? s_action_names[act] : "?";
}

/** @brief 大小写不敏感的字符串相等（避免依赖 strings.h 的 strcasecmp） */
static bool ci_equal(const char* a, const char* b) {
    if ((a == NULL) || (b == NULL)) {
        return false;
    }
    while ((*a != '\0') && (*b != '\0')) {
        char ca = *a;
        char cb = *b;

        if ((ca >= 'A') && (ca <= 'Z')) {
            ca = (char)(ca - 'A' + 'a');
        }
        if ((cb >= 'A') && (cb <= 'Z')) {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
        a++;
        b++;
    }
    return (*a == '\0') && (*b == '\0');
}

static int fault_code_parse(const char* text, uint32_t* code) {
    uint32_t hex = 0U;

    if ((text == NULL) || (code == NULL)) {
        return -1;
    }
    for (size_t i = 0U; i < (sizeof(s_fault_code_names) / sizeof(s_fault_code_names[0])); i++) {
        /* 大小写不敏感：故障码名一律按大写登记（OC_FAST_U），但台架习惯敲小写 */
        if (ci_equal(text, s_fault_code_names[i].name)) {
            *code = s_fault_code_names[i].code;
            return 0;
        }
    }
    {
        char* end = NULL;
        unsigned long v = strtoul(text, &end, 0); /* 支持 0x400 / 1024 */

        if ((end != NULL) && (*end == '\0') && (end != text)) {
            hex = (uint32_t)v;
            *code = hex;
            return 0;
        }
    }
    return -1;
}

static int fault_action_parse(const char* text, app_fault_action_t* act) {
    for (uint8_t i = 0U; i < 4U; i++) {
        if (strcmp(text, s_action_names[i]) == 0) {
            *act = (app_fault_action_t)i;
            return 0;
        }
    }
    return -1;
}

/**
 * @brief fault policy show | <code> <warn|derate|stop|trip> ；fault inject <code> on|off
 */
int app_terminal_cmd_fault_action_cmd(chry_shell_t* csh, int argc, char** argv) {
    if (strcmp(argv[1], "protect") == 0) {
        if (argc < 3) {
            csh_printf(csh, "actions=%s  (fault protect on|off)\r\n",
                       app_fault_actions_enabled() ? "ON (按 policy 执行动作)"
                                                   : "OFF (仅警告，不改写任何输出)");
            return 0;
        }
        if (strcmp(argv[2], "on") == 0) {
            app_fault_set_actions_enable(true);
        } else if (strcmp(argv[2], "off") == 0) {
            app_fault_set_actions_enable(false);
        } else {
            return app_terminal_cmd_usage(csh, "fault protect on|off");
        }
        csh_printf(csh,
                   "OK: actions=%s (RAM) | oc_fast=%s oc_slow=%s vbus_ov=%s adc_tmo=%s "
                   "enc_err=%s\r\n",
                   app_fault_actions_enabled() ? "ON" : "OFF",
                   s_action_names[(uint8_t)app_fault_get_policy(APP_FAULT_OC_FAST_U)],
                   s_action_names[(uint8_t)app_fault_get_policy(APP_FAULT_OC_SLOW_U)],
                   s_action_names[(uint8_t)app_fault_get_policy(APP_FAULT_VBUS_OV)],
                   s_action_names[(uint8_t)app_fault_get_policy(APP_FAULT_ADC_TIMEOUT)],
                   s_action_names[(uint8_t)app_fault_get_policy(APP_FAULT_ENC_READ)]);
        return 0;
    }
    if (strcmp(argv[1], "policy") == 0) {
        if ((argc < 3) || (strcmp(argv[2], "show") == 0)) {
            csh_printf(csh, "code        design   live\r\n");
            for (size_t i = 0U; i < (sizeof(s_fault_code_names) / sizeof(s_fault_code_names[0]));
                 i++) {
                uint32_t c = s_fault_code_names[i].code;

                csh_printf(csh, "%-11s %-8s %-8s\r\n", s_fault_code_names[i].name,
                           s_action_names[(uint8_t)app_fault_policy_default(c)],
                           s_action_names[(uint8_t)app_fault_get_policy(c)]);
            }
            csh_printf(csh, "action(now)=%s  limit_scale=%.2f\r\n",
                       s_action_names[(uint8_t)app_fault_get_action()],
                       (double)app_fault_get_limit_scale());
            return 0;
        }
        {
            uint32_t code = 0U;
            app_fault_action_t act;

            if ((fault_code_parse(argv[2], &code) != 0) || (argc < 4)
                || (fault_action_parse(argv[3], &act) != 0)) {
                return app_terminal_cmd_usage(csh, "fault policy <code> <warn|derate|stop|trip>");
            }
            app_fault_set_policy(code, act);
            csh_printf(csh, "OK: policy 0x%08X = %s (RAM)\r\n", (unsigned)code,
                       s_action_names[(uint8_t)act]);
            return 0;
        }
    }
    /* inject */
    if (argc < 4) {
        return app_terminal_cmd_usage(csh, "fault inject <code> on|off");
    }
    {
        uint32_t code = 0U;
        bool on;

        if (fault_code_parse(argv[2], &code) != 0) {
            return app_terminal_cmd_usage(csh, "fault inject <code> on|off");
        }
        if (strcmp(argv[3], "on") == 0) {
            on = true;
        } else if (strcmp(argv[3], "off") == 0) {
            on = false;
        } else {
            return app_terminal_cmd_usage(csh, "fault inject <code> on|off");
        }
        if (app_fault_inject(code, on) != 0) {
            csh_printf(csh, "FAIL: unknown code\r\n");
            return 0;
        }
        csh_printf(csh, "OK: inject 0x%08X %s | codes=0x%08X latched=0x%08X action=%s\r\n",
                   (unsigned)code, on ? "on" : "off", (unsigned)app_fault_get_codes(),
                   (unsigned)app_fault_get_latched(),
                   s_action_names[(uint8_t)app_fault_get_action()]);
    }
    return 0;
}
