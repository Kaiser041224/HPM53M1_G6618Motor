/**
 * @file    app_hardware_params.c
 * @brief   硬件参数访问器（工厂常量来自 build/generated/params_generated.c）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_hardware_params.h"

#include "params_generated.h"

#include <stdbool.h>
#include <stddef.h>

/** 运行期参数单例（boot 时 load；Shell param 命令可写） */
static app_hardware_params_t s_hardware_params_current;
static bool s_hardware_params_initialized;

const app_hardware_params_t* app_hardware_params_default(void) {
    return &g_hardware_params_factory;
}

void app_hardware_params_load(app_hardware_params_t* out) {
    if (out == NULL) {
        return;
    }
    *out = g_hardware_params_factory;
    /* TODO(v2): flash 覆盖 —— app_param_is_ready() → app_param_load(APP_PARAM_KEY_HARDWARE, ...) */
}

void app_hardware_params_init(void) {
    app_hardware_params_load(&s_hardware_params_current);
    s_hardware_params_initialized = true;
}

const app_hardware_params_t* app_hardware_params_current(void) {
    if (!s_hardware_params_initialized) {
        app_hardware_params_init();
    }
    return &s_hardware_params_current;
}

app_hardware_params_t* app_hardware_params_mutable(void) {
    if (!s_hardware_params_initialized) {
        app_hardware_params_init();
    }
    return &s_hardware_params_current;
}
