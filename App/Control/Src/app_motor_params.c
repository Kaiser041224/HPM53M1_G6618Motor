/**
 * @file    app_motor_params.c
 * @brief   电机参数访问器实现（工厂常量来自 build/generated/params_generated.c）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_motor_params.h"

#include "app_param.h"
#include "app_param_record.h"
#include "params_generated.h"

#include <stdbool.h>
#include <stddef.h>

/** 运行期参数单例（boot 时 load；Shell param 命令可写） */
static app_motor_params_t s_motor_params_current;
static bool s_motor_params_initialized;

const app_motor_params_t* app_motor_params_default(void) { return &g_motor_params_factory; }

int app_motor_params_load(app_motor_params_t* out) {
    if (out == NULL) {
        return -1;
    }
    *out = g_motor_params_factory;
    return app_param_record_load(
        APP_PARAM_KEY_MOTOR_PARAMS, out, sizeof(*out), g_software_params_factory.params_generation);
}

int app_motor_params_store(void) {
    /* 经 mutable() 懒初始化：未 init 直接 store 也不落全零记录 */
    const app_motor_params_t* current = app_motor_params_mutable();
    return app_param_record_store(
        APP_PARAM_KEY_MOTOR_PARAMS, current, sizeof(*current),
        g_software_params_factory.params_generation);
}

void app_motor_params_init(void) {
    app_motor_params_load(&s_motor_params_current);
    s_motor_params_initialized = true;
}

const app_motor_params_t* app_motor_params_current(void) {
    if (!s_motor_params_initialized) {
        app_motor_params_init();
    }
    return &s_motor_params_current;
}

app_motor_params_t* app_motor_params_mutable(void) {
    if (!s_motor_params_initialized) {
        app_motor_params_init();
    }
    return &s_motor_params_current;
}
