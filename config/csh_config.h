/*
 * Copyright (c) 2022, Egahp
 * Copyright (c) 2024-2026, HPMicro
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 本文件由 SDK 模板派生（middleware/cherrysh/port/hpm/barebone_usb/inc/csh_config.h），
 * 由 Alliance HardwareGroup 按本工程需求裁剪（值见各注释）。
 * 说明：csh.h 无条件 #include "csh_config.h"，由本工程 config/ 目录提供。
 * 约束：CONFIG_CSH_NOBLOCK=1 时必须 CONFIG_CSH_LNBUFF_STATIC=1，且 XTERM 必须为 0
 *       （CherryRL 编译期 #error 互斥）。
 */

#ifndef CSH_CONFIG_H
#define CSH_CONFIG_H

/*!< argument check（release：关闭参数检查） */
#define CONFIG_CSH_DEBUG 0

/*!< 终端默认行列 */
#define CONFIG_CSH_DFTROW 25
#define CONFIG_CSH_DFTCOL 80

/*!< 历史（↑/↓）；缓冲必须为 2 的幂 */
#define CONFIG_CSH_HISTORY 1
#define CONFIG_CSH_HISTORY_BUF_SIZE 256

/*!< TAB 补全 */
#define CONFIG_CSH_COMPLETION 1
#define CONFIG_CSH_MAX_COMPLETION 40

/*!< 彩色提示符 */
#define CONFIG_CSH_PROMPTEDIT 1
#define CONFIG_CSH_PROMPTEDIT_BUF_SIZE 128
#define CONFIG_CSH_PROMPTSEG 7

/*!< xterm 支持：与 NOBLOCK 互斥，固定 0 */
#define CONFIG_CSH_XTERM 0

/*!< 换行（串口终端约定） */
#define CONFIG_CSH_NEWLINE "\r\n"

/*!< TAB 空格数 */
#define CONFIG_CSH_SPACE 4

/*!< 独立键映射（不使用） */
#define CONFIG_CSH_CTRLMAP 0
#define CONFIG_CSH_ALTMAP 0

/*!< 刷新提示符 */
#define CONFIG_CSH_REFRESH_PROMPT 1

/*!< 非阻塞 sget（bare-metal 单循环必需） */
#define CONFIG_CSH_NOBLOCK 1

/*!< help 附加信息 */
#define CONFIG_CSH_HELP ""

/*!< 路径：0 = const 指针（无文件系统需求，省 128B RAM） */
#define CONFIG_CSH_MAXLEN_PATH 0
#define CONFIG_CSH_MAXSEG_PATH 16

/*!< 单用户、无登录 */
#define CONFIG_CSH_MAX_USER 1

/*!< 命令参数上限 */
#define CONFIG_CSH_MAX_ARG 8

/*!< 行缓冲静态分配（与 NOBLOCK 强制搭配） */
#define CONFIG_CSH_LNBUFF_STATIC 1
#define CONFIG_CSH_LNBUFF_SIZE 256

/*!< 单线程（bare-metal） */
#define CONFIG_CSH_MULTI_THREAD 0
#define CONFIG_CSH_SIGNAL_HANDLER 0

/*!< Ctrl-C/D/Q/S/Z、F1-F12 用户事件回调（Ctrl-C 中止 job 依赖此项） */
#define CONFIG_CSH_USER_CALLBACK 1

/*!< 命令表走链接段（FSymTab） */
#define CONFIG_CSH_SYMTAB 1

/*!< csh_printf 缓冲 */
#define CONFIG_CSH_PRINT_BUFFER_SIZE 512

#endif /* CSH_CONFIG_H */
