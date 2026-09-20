/*
 * App Motor Params - 电机参数访问器（工厂常量来自 build/generated/params_generated.c）
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_motor_params.h"

#include "params_generated.h"

#include <stddef.h>

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
