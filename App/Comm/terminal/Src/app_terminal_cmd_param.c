/**
 * @file    app_terminal_cmd_param.c
 * @brief   Terminal param 命令（元数据驱动：list / get / set / reset）
 * @author  Kaiser
 *
 * 名称空间：<域>.<路径>（域 = motor / hardware / software）
 *   例：hardware.current_sense.a_per_volt、software.control.current_loop.kp
 * 生效语义（元数据 apply 字段）：
 *   live   = 消费者逐次使用读取，set 立即生效
 *   reboot = init 期消费；v1 仅改 RAM 不持久化（flash 持久化见 v2）
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_terminal_cmd.h"

#include "app_hardware_params.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "params_meta_generated.h"

#include <math.h>
#include <string.h>

/** 安全相关参数（set 时警告；与设计文档 §8 一致） */
static const char* const s_safety_params[] = {
    "software.fault.oc_trip_a",        "software.fault.vbus_ov_v",
    "software.fault.vbus_uv_v",        "hardware.inverter.pwm_freq_hz",
    "hardware.inverter.deadtime_ns",   "software.control.limits.duty_max",
    "software.control.limits.i_q_max_a", "software.control.limits.i_trip_a",
    "hardware.current_sense.invert",
};

/** @brief apply 字段文本 */
static const char* param_apply_name(uint8_t apply) {
    return (apply == PARAM_META_APPLY_REBOOT) ? "reboot" : "live";
}

/** @brief 按名称查找元数据（名称须为域限定全名） */
static const param_meta_t* param_find(const char* name) {
    for (uint32_t i = 0U; i < g_params_meta_count; i++) {
        if (strcmp(g_params_meta[i].name, name) == 0) {
            return &g_params_meta[i];
        }
    }
    return NULL;
}

/** @brief 取参数当前值地址（可写单例 + 偏移） */
static void* param_data(const param_meta_t* meta) {
    uint8_t* base;

    switch (meta->domain) {
    case PARAM_META_DOMAIN_MOTOR: base = (uint8_t*)app_motor_params_mutable(); break;
    case PARAM_META_DOMAIN_HARDWARE: base = (uint8_t*)app_hardware_params_mutable(); break;
    default: base = (uint8_t*)app_software_params_mutable(); break;
    }
    return base + meta->offset;
}

/** @brief 取参数工厂值地址（只读常量 + 偏移） */
static const void* param_factory_data(const param_meta_t* meta) {
    const uint8_t* base;

    switch (meta->domain) {
    case PARAM_META_DOMAIN_MOTOR: base = (const uint8_t*)app_motor_params_default(); break;
    case PARAM_META_DOMAIN_HARDWARE: base = (const uint8_t*)app_hardware_params_default(); break;
    default: base = (const uint8_t*)app_software_params_default(); break;
    }
    return base + meta->offset;
}

/** @brief 参数类型字节数 */
static uint32_t param_type_size(uint8_t type) {
    switch (type) {
    case PARAM_META_TYPE_U8: return 1U;
    case PARAM_META_TYPE_U16: return 2U;
    default: return 4U;
    }
}

/** @brief 是否为安全相关参数 */
static bool param_is_safety_related(const char* name) {
    for (size_t i = 0U; i < sizeof(s_safety_params) / sizeof(s_safety_params[0]); i++) {
        if (strcmp(s_safety_params[i], name) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 打印单个参数（名称 = 值 [单位] (apply)）
 */
static void param_print(chry_shell_t* csh, const param_meta_t* meta) {
    const void* value = param_data(meta);
    const char* unit = (meta->unit != NULL) ? meta->unit : "";
    const char* apply = param_apply_name(meta->apply);

    switch (meta->type) {
    case PARAM_META_TYPE_U8:
        csh_printf(csh, "%-46s = %u %s (%s)\r\n", meta->name, (unsigned)*(const uint8_t*)value, unit,
                   apply);
        break;
    case PARAM_META_TYPE_U16:
        csh_printf(csh, "%-46s = %u %s (%s)\r\n", meta->name, (unsigned)*(const uint16_t*)value,
                   unit, apply);
        break;
    case PARAM_META_TYPE_U32:
        csh_printf(csh, "%-46s = %u %s (%s)\r\n", meta->name, (unsigned)*(const uint32_t*)value,
                   unit, apply);
        break;
    case PARAM_META_TYPE_F32:
    default:
        csh_printf(csh, "%-46s = %.7g %s (%s)\r\n", meta->name, (double)*(const float*)value, unit,
                   apply);
        break;
    }
}

/**
 * @brief 域过滤匹配（filter = NULL / "motor" / "hardware" / "software"）
 */
static bool param_matches_domain(const param_meta_t* meta, const char* filter) {
    size_t length;

    if (filter == NULL) {
        return true;
    }

    length = strlen(filter);
    if (strncmp(meta->name, filter, length) != 0) {
        return false;
    }

    /* 域边界：过滤串后必须是 '.'（域名）或串尾（域内全名） */
    return (meta->name[length] == '.') || (meta->name[length] == '\0');
}

/**
 * @brief param list [domain]
 */
static int cmd_param_list(chry_shell_t* csh, const char* filter) {
    uint32_t count = 0U;

    if ((filter != NULL) && (param_find(filter) == NULL)
        && (strcmp(filter, "motor") != 0) && (strcmp(filter, "hardware") != 0)
        && (strcmp(filter, "software") != 0)) {
        csh_printf(csh, "ERR: unknown domain '%s' (motor|hardware|software)\r\n", filter);
        return -1;
    }

    for (uint32_t i = 0U; i < g_params_meta_count; i++) {
        if (!param_matches_domain(&g_params_meta[i], filter)) {
            continue;
        }
        param_print(csh, &g_params_meta[i]);
        count++;
    }

    csh_printf(csh, "-- %u parameter(s)\r\n", (unsigned)count);
    return 0;
}

/**
 * @brief param get <name>
 */
static int cmd_param_get(chry_shell_t* csh, const char* name) {
    const param_meta_t* meta = param_find(name);

    if (meta == NULL) {
        csh_printf(csh, "ERR: unknown parameter '%s' (try 'param list')\r\n", name);
        return -1;
    }

    param_print(csh, meta);
    return 0;
}

/**
 * @brief param set <name> <value>
 */
static int cmd_param_set(chry_shell_t* csh, const char* name, const char* text) {
    const param_meta_t* meta = param_find(name);
    void* data;

    if (meta == NULL) {
        csh_printf(csh, "ERR: unknown parameter '%s' (try 'param list')\r\n", name);
        return -1;
    }

    data = param_data(meta);
    switch (meta->type) {
    case PARAM_META_TYPE_U8: {
        uint32_t value;

        if ((app_terminal_cmd_parse_u32(text, &value) != 0) || (value > 0xFFU)) {
            csh_printf(csh, "ERR: invalid u8 value '%s'\r\n", text);
            return -1;
        }
        if (((float)value < meta->min) || ((float)value > meta->max)) {
            goto out_of_range;
        }
        *(uint8_t*)data = (uint8_t)value;
        break;
    }
    case PARAM_META_TYPE_U16: {
        uint32_t value;

        if ((app_terminal_cmd_parse_u32(text, &value) != 0) || (value > 0xFFFFU)) {
            csh_printf(csh, "ERR: invalid u16 value '%s'\r\n", text);
            return -1;
        }
        if (((float)value < meta->min) || ((float)value > meta->max)) {
            goto out_of_range;
        }
        *(uint16_t*)data = (uint16_t)value;
        break;
    }
    case PARAM_META_TYPE_U32: {
        uint32_t value;

        if (app_terminal_cmd_parse_u32(text, &value) != 0) {
            csh_printf(csh, "ERR: invalid u32 value '%s'\r\n", text);
            return -1;
        }
        if (((float)value < meta->min) || ((float)value > meta->max)) {
            goto out_of_range;
        }
        *(uint32_t*)data = value;
        break;
    }
    case PARAM_META_TYPE_F32:
    default: {
        float value;

        if (app_terminal_cmd_parse_float(text, &value) != 0) {
            csh_printf(csh, "ERR: invalid float value '%s'\r\n", text);
            return -1;
        }
        if (!isfinite(value) || (value < meta->min) || (value > meta->max)) {
            goto out_of_range;
        }
        *(float*)data = value;
        break;
    }
    }

    if (param_is_safety_related(name)) {
        csh_printf(csh, "WARN: safety-related parameter\r\n");
    }
    if (meta->apply == PARAM_META_APPLY_REBOOT) {
        csh_printf(csh, "OK  %s = %s (RAM; consumed at boot, not persistent - v2: save)\r\n", name,
                   text);
    } else {
        csh_printf(csh, "OK  %s = %s (RAM, live)\r\n", name, text);
    }
    return 0;

out_of_range:
    csh_printf(csh, "ERR: %s out of range [%.7g, %.7g]\r\n", name, (double)meta->min,
               (double)meta->max);
    return -1;
}

/**
 * @brief param reset [name|domain]
 */
static int cmd_param_reset(chry_shell_t* csh, const char* target) {
    if (target == NULL) {
        *app_motor_params_mutable() = *app_motor_params_default();
        *app_hardware_params_mutable() = *app_hardware_params_default();
        *app_software_params_mutable() = *app_software_params_default();
        csh_printf(csh, "OK  all parameters reset to factory defaults\r\n");
        return 0;
    }

    if (strcmp(target, "motor") == 0) {
        *app_motor_params_mutable() = *app_motor_params_default();
        csh_printf(csh, "OK  motor parameters reset to factory defaults\r\n");
        return 0;
    }
    if (strcmp(target, "hardware") == 0) {
        *app_hardware_params_mutable() = *app_hardware_params_default();
        csh_printf(csh, "OK  hardware parameters reset to factory defaults\r\n");
        return 0;
    }
    if (strcmp(target, "software") == 0) {
        *app_software_params_mutable() = *app_software_params_default();
        csh_printf(csh, "OK  software parameters reset to factory defaults\r\n");
        return 0;
    }

    {
        const param_meta_t* meta = param_find(target);

        if (meta == NULL) {
            csh_printf(csh, "ERR: unknown parameter '%s' (try 'param list')\r\n", target);
            return -1;
        }
        memcpy(param_data(meta), param_factory_data(meta), param_type_size(meta->type));
        csh_printf(csh, "OK  %s reset to factory default\r\n", target);
    }
    return 0;
}

/**
 * @brief param 命令入口
 */
static int cmd_param(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);
    const char* usage =
        "param list [domain] | param get <name> | param set <name> <value> | param reset "
        "[name|domain]";

    if (argc < 2) {
        return app_terminal_cmd_usage(csh, usage);
    }
    if (strcmp(argv[1], "list") == 0) {
        return cmd_param_list(csh, (argc >= 3) ? argv[2] : NULL);
    }
    if (strcmp(argv[1], "get") == 0) {
        if (argc < 3) {
            return app_terminal_cmd_usage(csh, usage);
        }
        return cmd_param_get(csh, argv[2]);
    }
    if (strcmp(argv[1], "set") == 0) {
        if (argc < 4) {
            return app_terminal_cmd_usage(csh, usage);
        }
        return cmd_param_set(csh, argv[2], argv[3]);
    }
    if (strcmp(argv[1], "reset") == 0) {
        return cmd_param_reset(csh, (argc >= 3) ? argv[2] : NULL);
    }

    return app_terminal_cmd_usage(csh, usage);
}
CSH_CMD_EXPORT_ALIAS(cmd_param, param, );
