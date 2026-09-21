/**
 * @file    app_software_params.c
 * @brief   软件参数访问器（工厂常量来自 build/generated/params_generated.c）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_software_params.h"

#include "params_generated.h"

#include <stddef.h>

const app_software_params_t* app_software_params_default(void) {
    return &g_software_params_factory;
}

void app_software_params_load(app_software_params_t* out) {
    if (out == NULL) {
        return;
    }
    *out = g_software_params_factory;
    /* TODO(v2): flash 覆盖 —— app_param_is_ready() → app_param_load(APP_PARAM_KEY_SOFTWARE,
     * ...)（整定/阈值微调） */
}
