/**
 * @file    app_terminal_cmd.c
 * @brief   Terminal 命令公共工具实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_terminal_cmd.h"

#include "app_debug_motor.h"
#include "app_debug_rtt.h"
#include "app_fault.h"
#include "app_terminal.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
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
    if (app_debug_motor_is_running()) {
        csh_printf(csh, "ERR: motor is running, stop it first (motor stop)\r\n");
        return false;
    }
    return true;
}

bool app_terminal_cmd_require_no_fault(chry_shell_t* csh) {
    uint32_t latched = app_fault_get_latched();

    if (latched != 0U) {
        csh_printf(csh, "ERR: fault latched 0x%08X, clear it first (fault clear)\r\n",
                   (unsigned)latched);
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
