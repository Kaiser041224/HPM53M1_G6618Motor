/*
 * App HW Params - 硬件参数访问器（工厂常量来自 build/generated/params_generated.c）
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_hw_params.h"

#include "params_generated.h"

#include <stddef.h>

const app_hw_params_t *app_hw_params_default(void) {
    return &g_hw_params_factory;
}

void app_hw_params_load(app_hw_params_t *out) {
    if (out == NULL) {
        return;
    }
    *out = g_hw_params_factory;
    /* TODO(v2): flash 覆盖 —— app_param_is_ready() → app_param_load(APP_PARAM_KEY_HW, ...) */
}
