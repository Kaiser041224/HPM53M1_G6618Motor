/*
 * Debug Fault - 故障保护调试（f：打印 / F：清除锁存）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_DEBUG_FAULT_H
#define APP_DEBUG_FAULT_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 命令 f：打印故障状态机 / 故障码 / 计数 / 首故障快照 */
void app_debug_fault_dump(void);

/** @brief 命令 F：清除锁存（条件须已恢复） */
void app_debug_fault_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_DEBUG_FAULT_H */
