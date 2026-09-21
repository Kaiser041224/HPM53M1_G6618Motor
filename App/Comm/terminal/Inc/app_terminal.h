/**
 * @file    app_terminal.h
 * @brief   USB CDC 终端（CherrySH 绑定：会话 / 输出缓冲 / 回调链）
 * @author  Kaiser
 *
 * 职责：
 *   - CherrySH 实例生命周期（init + 1kHz 轮询）
 *   - 移植层：sput（→ TX 环 → USB 非阻塞 flush）/ sget（← USB 驱动环）
 *   - 回调链：补全（命令/子命令/参数名 + 重复 TAB 循环）/ 用户事件（Ctrl-C → job/monitor）
 *   - DTR 上升沿：清 TX 环 + banner + 提示符重绘
 *
 * 约束：
 *   - 全部逻辑运行在 1kHz 慢任务，不在 ISR 上下文执行；
 *   - 输出门控用 DTR（主机未打开端口时丢弃，不累积陈旧输出）；
 *   - 命令本体必须快速返回，长流程走 app_terminal_job。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_TERMINAL_H
#define APP_TERMINAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 Terminal（在 app_usb_init() 之后调用；不等待枚举/DTR，不阻塞）。
 */
void app_terminal_init(void);

/**
 * @brief 1kHz 轮询：命令执行 + 输入处理 + job tick + DTR 边沿 + TX flush。
 */
void app_terminal_run_once(void);

/**
 * @brief 向终端输出（与 sput 同路径：TX 环 + DTR 门控 + 有界 flush）。
 * @param data 数据
 * @param len 长度
 *
 * 用途：命令输出 / dump 旁路写入器（app_debug_set_writer）。
 */
void app_terminal_write(const void* data, size_t len);

/**
 * @brief 重绘提示符（job 输出/结束后恢复行编辑显示状态）。
 */
void app_terminal_refresh(void);

/**
 * @brief Terminal 是否初始化成功。
 * @return true = 就绪
 */
bool app_terminal_is_ready(void);

/**
 * @brief TX 丢弃字节数（主机未打开 / 背压极端时的输出丢失统计）。
 * @return 累计丢弃字节数
 */
uint32_t app_terminal_get_tx_drop_bytes(void);

/**
 * @brief 输出流换行计数（monitor 用于判断"是否有输出发生"）。
 * @return 累计 '\n' 个数
 */
uint32_t app_terminal_get_line_feeds(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_TERMINAL_H */
