/**
 * @file    tusb_config.h
 * @brief   TinyUSB 配置（G6618 DFU bootloader）
 * @author  Kaiser
 *
 * 仅启用 DFU 设备类；高速枚举（HPM53M1 集成 HS PHY，与应用侧的 CherryUSB 一致）。
 * WCID/MS OS 2.0 由 dfu_desc.c 提供，Windows 10+ 免驱绑定 WinUSB。
 *
 * 注意：本工程不链接 SDK 的 hw/bsp（其 board_millis 使用 rdcycle），
 * 时基与 USB ISR 由 dfu.c / 本工程提供。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_MCU       OPT_MCU_HPM
#define CFG_TUSB_OS        OPT_OS_NONE
#define CFG_TUSB_DEBUG     0

#define CFG_TUD_ENABLED    1
#define CFG_TUD_MAX_SPEED  OPT_MODE_HIGH_SPEED

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION __attribute__((section(".noncacheable.non_init")))
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_CDC      0
#define CFG_TUD_MSC      0
#define CFG_TUD_HID      0
#define CFG_TUD_MIDI     0
#define CFG_TUD_VENDOR   0

#define CFG_TUD_DFU      1
#define CFG_TUD_DFU_XFER_BUFSIZE 1024

#endif /* TUSB_CONFIG_H */
