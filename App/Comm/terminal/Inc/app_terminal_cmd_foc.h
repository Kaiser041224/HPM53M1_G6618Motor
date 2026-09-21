/**
 * @file    app_terminal_cmd_foc.h
 * @brief   Terminal FOC 命令（foc status/on/off）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_TERMINAL_CMD_FOC_H
#define APP_TERMINAL_CMD_FOC_H

#include "app_foc.h"

#include "csh.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FOC 状态名
 * @param state 状态值
 * @return 名称；越界返回 "?"
 */
const char* app_terminal_cmd_foc_state_name(app_foc_state_t state);

/**
 * @brief 打印 FOC 状态块（state/θe/ωe/id/iq/vd/vq/duty/valid/counters）
 * @param csh terminal 实例
 */
void app_terminal_cmd_foc_status(chry_shell_t* csh);

#ifdef __cplusplus
}
#endif

#endif /* APP_TERMINAL_CMD_FOC_H */
