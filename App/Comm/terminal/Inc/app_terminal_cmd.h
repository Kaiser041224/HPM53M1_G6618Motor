/**
 * @file    app_terminal_cmd.h
 * @brief   Terminal 命令公共工具（上下文 / 参数解析 / 安全联锁 / dump 旁路）
 * @author  Kaiser
 *
 * 命令实现模板（详见设计文档 §5.3）：
 *   static int cmd_xxx(int argc, char **argv) {
 *       chry_shell_t *csh = app_terminal_cmd_ctx(argc, argv);
 *       ...
 *       return 0;
 *   }
 *   CSH_CMD_EXPORT_ALIAS(cmd_xxx, xxx, );
 * （本 SDK CherrySH 版本无 CSH_CMD_EXPORT_FULL；usage 由命令自身打印）
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_TERMINAL_CMD_H
#define APP_TERMINAL_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "csh.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 取命令上下文（CherrySH 约定：argv[argc + 1] 存 terminal 实例指针）。
 * @param argc 参数个数
 * @param argv 参数数组
 * @return terminal 实例
 */
static inline chry_shell_t* app_terminal_cmd_ctx(int argc, char** argv) {
    return (chry_shell_t*)(void*)argv[argc + 1];
}

/**
 * @brief 输出 usage 并返回命令错误码。
 * @param csh terminal 实例
 * @param usage 用法字符串
 * @return -1（供命令直接 return）
 */
int app_terminal_cmd_usage(chry_shell_t* csh, const char* usage);

/**
 * @brief 解析无符号整数（支持 0x 前缀十六进制）。
 * @param text 文本
 * @param out 输出值
 * @return 0 = 成功；-1 = 非法
 */
int app_terminal_cmd_parse_u32(const char* text, uint32_t* out);

/**
 * @brief 解析有符号整数。
 * @param text 文本
 * @param out 输出值
 * @return 0 = 成功；-1 = 非法
 */
int app_terminal_cmd_parse_i32(const char* text, int32_t* out);

/**
 * @brief 解析浮点数。
 * @param text 文本
 * @param out 输出值
 * @return 0 = 成功；-1 = 非法
 */
int app_terminal_cmd_parse_float(const char* text, float* out);

/**
 * @brief 安全联锁：要求电机停止（旋转中打印错误并拒绝）。
 * @param csh terminal 实例
 * @return true = 允许执行；false = 拒绝
 */
bool app_terminal_cmd_require_motor_stopped(chry_shell_t* csh);

/**
 * @brief 安全联锁：要求无故障锁存（锁存中打印错误并拒绝）。
 * @param csh terminal 实例
 * @return true = 允许执行；false = 拒绝
 */
bool app_terminal_cmd_require_no_fault(chry_shell_t* csh);

/**
 * @brief dump 旁路：执行期把 Debug 层 dump 输出同时送入终端（不影响 RTT）。
 * @param dump_fn dump 函数（内部经 app_debug_printf 输出）
 */
void app_terminal_cmd_run_dump(void (*dump_fn)(void));

/**
 * @brief 开始捕获：把 app_debug_printf 输出同时送入终端（配对 capture_end）。
 *
 * 用途：包装多段调用（如 motor/inv 的既有打印）使其在终端可见。
 */
void app_terminal_cmd_capture_begin(void);

/**
 * @brief 结束捕获（摘除旁路写入器）。
 */
void app_terminal_cmd_capture_end(void);

/**
 * @brief 故障状态机状态名（与 app_fault_state_t 对应）。
 * @param state 状态值
 * @return 名称；越界返回 "?"
 */
const char* app_terminal_cmd_fault_state_name(uint8_t state);

/**
 * @brief 故障位图 → 名称文本（"OC_SLOW_U|VBUS_OV"；0 → "none"）。
 * @param codes 故障位图
 * @param out 输出缓冲
 * @param out_size 缓冲大小
 */
void app_terminal_cmd_fault_codes_text(uint32_t codes, char* out, size_t out_size);

/**
 * @brief 输出格式化文本到终端（job 回调等无 csh 上下文的路径）。
 * @param format 格式串
 * @param ... 可变参数
 */
void app_terminal_cmd_emit(const char* format, ...);

#ifdef __cplusplus
}
#endif

#endif /* APP_TERMINAL_CMD_H */
