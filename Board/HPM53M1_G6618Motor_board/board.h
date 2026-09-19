/*
 * Copyright (c) 2024 HPMicro
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _HPM_BOARD_H
#define _HPM_BOARD_H

#include "hpm_soc.h"

#define BOARD_NAME          "HPM53M1_G6618Motor_board"
#define BOARD_UF2_SIGNATURE (0x0A4D5048UL)

#define SEC_CORE_IMG_START ILM_LOCAL_BASE

/* ============================================================================
 * Flash（XPI NOR，1MB）
 *   末尾 BOARD_FLASH_RESERVED_SIZE 由链接脚本预留：
 *     倒数第 2 扇区 = 参数区（app_param），最后 1 扇区 = 自检/测试用
 * ============================================================================ */
#define BOARD_APP_XPI_NOR_XPI_BASE     (HPM_XPI0)
#define BOARD_APP_XPI_NOR_BASE_ADDR    (0x80000000U)
#define BOARD_APP_XPI_NOR_CFG_OPT_HDR  (0xfcf90002U)
#define BOARD_APP_XPI_NOR_CFG_OPT_OPT0 (0x00000006U)
#define BOARD_APP_XPI_NOR_CFG_OPT_OPT1 (0x00001000U)
#define BOARD_FLASH_RESERVED_SIZE      (0x2000U) /* 2 × 4KB 扇区 */

#ifndef BOARD_RUNNING_CORE
#define BOARD_RUNNING_CORE HPM_CORE0
#endif


#if defined(__cplusplus)
extern "C" {
#endif /* __cplusplus */

void board_init(void);
void board_init_usb(void);
void board_init_core1(void);

#if defined(__cplusplus)
}
#endif /* __cplusplus */
#endif /* _HPM_BOARD_H */
