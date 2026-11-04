/**
 * @file    app_param_meta.c
 * @brief   参数元数据共享访问实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_param_meta.h"

#include "app_hardware_params.h"
#include "app_motor_params.h"
#include "app_software_params.h"

#include <string.h>

const param_meta_t *app_param_meta_find(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (uint32_t i = 0U; i < g_params_meta_count; i++) {
        if (strcmp(g_params_meta[i].name, name) == 0) {
            return &g_params_meta[i];
        }
    }
    return NULL;
}

void *app_param_meta_data(const param_meta_t *meta) {
    uint8_t *base;

    if (meta == NULL) {
        return NULL;
    }
    switch (meta->domain) {
    case PARAM_META_DOMAIN_MOTOR: base = (uint8_t *)app_motor_params_mutable(); break;
    case PARAM_META_DOMAIN_HARDWARE: base = (uint8_t *)app_hardware_params_mutable(); break;
    default: base = (uint8_t *)app_software_params_mutable(); break;
    }
    return base + meta->offset;
}

const void *app_param_meta_factory_data(const param_meta_t *meta) {
    const uint8_t *base;

    if (meta == NULL) {
        return NULL;
    }
    switch (meta->domain) {
    case PARAM_META_DOMAIN_MOTOR: base = (const uint8_t *)app_motor_params_default(); break;
    case PARAM_META_DOMAIN_HARDWARE:
        base = (const uint8_t *)app_hardware_params_default();
        break;
    default: base = (const uint8_t *)app_software_params_default(); break;
    }
    return base + meta->offset;
}

uint32_t app_param_meta_type_size(uint8_t type) {
    switch (type) {
    case PARAM_META_TYPE_U8: return 1U;
    case PARAM_META_TYPE_U16: return 2U;
    default: return 4U;
    }
}
