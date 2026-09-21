/**
 * @file    app_motor_params.c
 * @brief   电机参数访问器实现（工厂常量来自 build/generated/params_generated.c）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_motor_params.h"

#include "params_generated.h"

#include <stdbool.h>
#include <stddef.h>

/** 运行期参数单例（boot 时 load；Shell param 命令可写） */
static app_motor_params_t s_motor_params_current;
static bool s_motor_params_initialized;

const app_motor_params_t *app_motor_params_default(void) {
    return &g_motor_params_factory;
}

void app_motor_params_load(app_motor_params_t *out) {
    if (out == NULL) {
        return;
    }
    *out = g_motor_params_factory;
    /* TODO(v2): flash 覆盖 —— app_param_is_ready() → app_param_load(APP_PARAM_KEY_MOTOR, ...)（在线辨识结果） */
}

void app_motor_params_init(void) {
    app_motor_params_load(&s_motor_params_current);
    s_motor_params_initialized = true;
}

const app_motor_params_t *app_motor_params_current(void) {
    if (!s_motor_params_initialized) {
        app_motor_params_init();
    }
    return &s_motor_params_current;
}

app_motor_params_t *app_motor_params_mutable(void) {
    if (!s_motor_params_initialized) {
        app_motor_params_init();
    }
    return &s_motor_params_current;
}
