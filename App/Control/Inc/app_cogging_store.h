/**
 * @file    app_cogging_store.h
 * @brief   齿槽/传动角表持久化（独立 flash 扇区）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_COGGING_STORE_H
#define APP_COGGING_STORE_H

#include "app_cogging.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 从独立角表扇区加载；0=有效记录，-1=无/无效 */
int app_cogging_store_load(void);
/** @brief 保存角表；仅 FOC OFF 停机窗口；0=成功 */
int app_cogging_store_save(void);
/** @brief 清除角表扇区；仅 FOC OFF 停机窗口；0=成功 */
int app_cogging_store_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_COGGING_STORE_H */
