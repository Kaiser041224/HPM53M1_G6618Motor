/**
 * @file    app_param_meta.h
 * @brief   参数元数据共享访问（Terminal 与二进制协议共用）
 * @author  Kaiser
 *
 * 元数据表由 scripts/gen_params.py 生成（params_meta_generated.[ch]）。
 * 本模块只提供「按名查找 + 取值地址 + 类型宽度」，不承担解析/打印/联锁：
 *   - Terminal 的文本解析与打印留在 App/Comm/terminal；
 *   - 二进制协议的线格式映射留在 App/Comm/usb。
 * 两处必须共用同一套查找与地址计算，避免各自实现漂移。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef APP_PARAM_META_H
#define APP_PARAM_META_H

#include "params_meta_generated.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 按域限定全名查找元数据
 * @param name 参数名（如 "motor.pole_pairs"）
 * @return 元数据指针；NULL = 未找到
 */
const param_meta_t *app_param_meta_find(const char *name);

/**
 * @brief 取参数当前值的地址（可写单例 + 偏移）
 * @param meta 元数据
 * @return 参数值地址
 */
void *app_param_meta_data(const param_meta_t *meta);

/**
 * @brief 取参数出厂默认值的地址（只读常量 + 偏移）
 * @param meta 元数据
 * @return 默认值地址
 */
const void *app_param_meta_factory_data(const param_meta_t *meta);

/**
 * @brief 参数类型字节宽度
 * @param type param_meta_type_t
 * @return 1 / 2 / 4
 */
uint32_t app_param_meta_type_size(uint8_t type);

#ifdef __cplusplus
}
#endif

#endif /* APP_PARAM_META_H */
