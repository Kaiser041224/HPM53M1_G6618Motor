/**
 * @file    app_cold_ram.h
 * @brief   冷数据放置属性（AHB SRAM / .ahb_sram）
 * @author  Kaiser
 *
 * 仅用于非控制热路径的大缓冲区（终端、flash 参数暂存、标定/调试）。
 * FOC/motion 热状态、ISR 数据、USB/CAN DMA 缓冲仍应留在 DLM 或
 * .noncacheable，不能使用本属性。
 *
 * .ahb_sram 为 NOLOAD 段：启动阶段不会自动清零；使用方必须在 init()
 * 或用前初始化。若改成 DMA 缓冲，必须重新评估缓存/一致性约束。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_COLD_RAM_H
#define APP_COLD_RAM_H

/** 冷数据放入 AHB SRAM（.ahb_sram.bss；NOLOAD，使用前须初始化） */
#define APP_ATTR_AHB_SRAM __attribute__((section(".ahb_sram.bss"), aligned(4)))

#endif /* APP_COLD_RAM_H */
