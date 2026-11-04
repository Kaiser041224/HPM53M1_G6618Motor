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

_Static_assert(
    sizeof(app_motor_params_t) + sizeof(app_param_record_header_t) <= APP_PARAM_DATA_MAX,
    "motor params record exceeds app_param slot");

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
    /* load_inertia 追加在结构体末尾：旧记录按前缀迁移，新字段保持 YAML 出厂默认。 */
    return app_param_record_load_prefix(
        APP_PARAM_KEY_MOTOR_PARAMS, out, sizeof(*out), APP_MOTOR_PARAMS_V1_SIZE, 1U);
}

int app_motor_params_store(void) {
    /* 经 mutable() 懒初始化：未 init 直接 store 也不落全零记录 */
    const app_motor_params_t* current = app_motor_params_mutable();
    return app_param_record_store(APP_PARAM_KEY_MOTOR_PARAMS, current, sizeof(*current), 1U);
}

int app_motor_params_reset(void) {
    *app_motor_params_mutable() = *app_motor_params_default();
    return app_motor_params_store();
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
