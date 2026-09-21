/**
 * @file    app_debug_cmd.h
 * @brief   调试控制台单字符命令（UART / USB 共用）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_DEBUG_CMD_H
#define APP_DEBUG_CMD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 处理接收到的控制台数据（逐字节匹配命令）。
 *
 * 命令：
 *   z = 设置转子零点（软件，存 flash）
 *   o = 设置出轴零点（软件，存 flash）
 *   c = 清除两路零点
 *   i = 打印零点与当前位置
 */
void app_debug_cmd_handle(const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* APP_DEBUG_CMD_H */
