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
/* 追加字段迁移基准：旧记录（到 storage 为止）必须能被前缀拷贝接受 */
#define APP_SOFTWARE_PARAMS_V1_SIZE (offsetof(app_software_params_t, speed_observer))
_Static_assert(
    APP_SOFTWARE_PARAMS_V1_SIZE > 0U && APP_SOFTWARE_PARAMS_V1_SIZE < sizeof(app_software_params_t),
    "new software params fields must be appended after storage");

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
    /* reset/override 都忽略 flash；keep 才加载旧版前缀记录。
     * 记录头仍用固定 generation=1，旧固件保存的记录保持兼容。 */
    if (g_software_params_factory.storage != APP_SW_STORAGE_KEEP) {
        return 0;
    }
    int rc = app_param_record_load_prefix(
        APP_PARAM_KEY_SOFTWARE_PARAMS, out, sizeof(*out), APP_SOFTWARE_PARAMS_V1_SIZE, 1U);
    out->storage = g_software_params_factory.storage;
    return rc;
}

int app_software_params_store(void) {
    /* 经 mutable() 懒初始化：未 init 直接 store 也不落全零记录 */
    const app_software_params_t* current = app_software_params_mutable();
    return app_param_record_store(
        APP_PARAM_KEY_SOFTWARE_PARAMS, current, sizeof(*current), 1U);
}

void app_software_params_init(void) {
    app_software_params_load(&s_software_params_current);
    s_software_params_initialized = true;
    /* reset：本次启动已用 YAML 默认，回写一条默认记录；
     * 否则下次 keep 还会看到旧 flash。override 不回写。
     * MPRM/FCUR 的清理由 app_logic 在 flash 就绪后统一编排。 */
    if (g_software_params_factory.storage == APP_SW_STORAGE_RESET) {
        (void)app_software_params_store();
    }
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
