/**
 * @file    app_software_params.c
 * @brief   软件参数访问器（工厂常量来自 build/generated/params_generated.c）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_software_params.h"

#include "app_param.h"
#include "app_param_record.h"
#include "app_ramfunc.h"
#include "params_generated.h"

#include <stdbool.h>
#include <stddef.h>

_Static_assert(
    sizeof(app_software_params_t) + sizeof(app_param_record_header_t) <= APP_PARAM_DATA_MAX,
    "software params record exceeds app_param slot");
/* 追加字段迁移基准：旧记录（到 params_generation 为止）必须能被前缀拷贝接受 */
#define APP_SOFTWARE_PARAMS_V1_SIZE (offsetof(app_software_params_t, speed_observer))
_Static_assert(
    APP_SOFTWARE_PARAMS_V1_SIZE > 0U && APP_SOFTWARE_PARAMS_V1_SIZE < sizeof(app_software_params_t),
    "new software params fields must be appended after params_generation");

/** 运行期参数单例（boot 时 load；Shell param 命令可写） */
static app_software_params_t s_software_params_current;
static bool s_software_params_initialized;

const app_software_params_t* app_software_params_default(void) {
    return &g_software_params_factory;
}

int app_software_params_load(app_software_params_t* out) {
    if (out == NULL) {
        return -1;
    }
    *out = g_software_params_factory;
    /* 先按旧版前缀长度接受记录：旧字段叠加，新增字段保留出厂默认。
     * 这样 params_generation 不变时升级固件不会静默丢弃已保存的运动整定。 */
    return app_param_record_load_prefix(
        APP_PARAM_KEY_SOFTWARE_PARAMS, out, sizeof(*out), APP_SOFTWARE_PARAMS_V1_SIZE,
        g_software_params_factory.params_generation);
}

int app_software_params_store(void) {
    /* 经 mutable() 懒初始化：未 init 直接 store 也不落全零记录 */
    const app_software_params_t* current = app_software_params_mutable();
    return app_param_record_store(
        APP_PARAM_KEY_SOFTWARE_PARAMS, current, sizeof(*current),
        g_software_params_factory.params_generation);
}

void app_software_params_init(void) {
    app_software_params_load(&s_software_params_current);
    s_software_params_initialized = true;
}

APP_ATTR_RAMFUNC /* FOC ISR 读取（电流环增益/限幅） */
    const app_software_params_t*
    app_software_params_current(void) {
    if (!s_software_params_initialized) {
        app_software_params_init();
    }
    return &s_software_params_current;
}

app_software_params_t* app_software_params_mutable(void) {
    if (!s_software_params_initialized) {
        app_software_params_init();
    }
    return &s_software_params_current;
}
