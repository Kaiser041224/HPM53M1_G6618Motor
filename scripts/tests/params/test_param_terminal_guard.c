/**
 * @file    test_param_terminal_guard.c
 * @brief   Terminal param foc_off 守卫：直接编译命令实现并验证拒绝/接受路径
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_foc.h"
#include "app_motor_params.h"
#include "app_terminal_cmd_param.c"

#include <stdarg.h>
#include <stdlib.h>

static app_foc_state_t s_foc_state;

/** @brief app_foc 层替身：本测试只验证终端守卫，不驱动 FOC */
app_foc_state_t app_foc_get_state(void) { return s_foc_state; }
bool app_foc_stop_pending(void) { return false; }

/** @brief CherrySH 输出替身 */
int csh_printf(chry_shell_t* csh, const char* fmt, ...) {
    (void)csh;
    (void)fmt;
    return 0;
}

int app_terminal_cmd_usage(chry_shell_t* csh, const char* usage) {
    (void)csh;
    (void)usage;
    return -1;
}

int app_terminal_cmd_parse_u32(const char* text, uint32_t* out) {
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 0);

    if ((end == text) || (*end != '\0')) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

int app_terminal_cmd_parse_float(const char* text, float* out) {
    char* end = NULL;
    float value = strtof(text, &end);

    if ((end == text) || (*end != '\0')) {
        return -1;
    }
    *out = value;
    return 0;
}

/** @return 0 = 通过；非0 = 失败数 */
int test_param_terminal_guard(void) {
    int fails = 0;

    app_motor_params_init();
    *app_motor_params_mutable() = *app_motor_params_default();
    s_foc_state = APP_FOC_STATE_RUN;
    float before = app_motor_params_mutable()->rs_ohm;

    /* FOC 非 OFF：foc_off 字段必须拒绝，且不得写入 RAM */
    if (cmd_param_set(NULL, "motor.rs_ohm", "0.5") == 0) {
        fails++;
    }
    if (app_motor_params_mutable()->rs_ohm != before) {
        fails++;
    }

    /* FOC OFF：接受写入，下一次 foc on 由模型重载消费 */
    s_foc_state = APP_FOC_STATE_OFF;
    if (cmd_param_set(NULL, "motor.rs_ohm", "0.5") != 0) {
        fails++;
    }
    if (app_motor_params_mutable()->rs_ohm != 0.5f) {
        fails++;
    }

    /* live 字段不受 FOC 状态守卫影响 */
    s_foc_state = APP_FOC_STATE_RUN;
    if (cmd_param_set(NULL, "software.control.current_loop.kp", "2.0") != 0) {
        fails++;
    }
    if (app_software_params_current()->control.current_loop.kp != 2.0f) {
        fails++;
    }

    *app_motor_params_mutable() = *app_motor_params_default();
    *app_software_params_mutable() = *app_software_params_default();
    return fails;
}
