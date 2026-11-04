/**
 * @file    app_ramfunc.h
 * @brief   App 层热路径段属性（ILM / .fast）
 * @author  Kaiser
 *
 * 等价 SDK ATTR_RAMFUNC（hpm_common.h：ATTR_PLACE_AT(".fast")），供 App 层使用而不包含
 * hpm_* 头文件。链接脚本 .fast 段位于 ILM（linkers/gcc/user_linker.ld）。
 *
 * 使用范围：25kHz ADC0 PMT 完成中断（FOC 快车道）调用链上的函数。
 * 非热路径函数不要标注（ILM 128KB，保留给实时代码）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_RAMFUNC_H
#define APP_RAMFUNC_H

/** 函数放入 ILM（.fast 段，启动时由 flash 拷贝） */
#define APP_ATTR_RAMFUNC __attribute__((section(".fast")))

/** 热路径小常量表放入 DLM（.fast_ram.init 段，启动时由 flash 拷贝；避免 ISR 读 XIP） */
#define APP_ATTR_FAST_RAM_INIT __attribute__((section(".fast_ram.init")))

#endif /* APP_RAMFUNC_H */
