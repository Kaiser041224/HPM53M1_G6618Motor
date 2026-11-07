/**
 * @file    flash.h
 * @brief   Bootloader XPI NOR 访问 + DFU 扇区缓冲写入器
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BOOT_FLASH_H
#define BOOT_FLASH_H

#include "intf_boot.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief 初始化 XPI NOR（ROM auto config；必须在时钟初始化之后） */
void boot_flash_init(void);

/** @brief 元数据日志使用的 flash 后端 */
const boot_flash_ops_t *boot_flash_ops(void);

/** @brief 直接读 flash（XIP，不关中断；用于头/元数据小对象） */
int boot_flash_read(uint32_t address, void *buf, size_t len);

/** @brief 擦除 4KB 扇区（关全局中断 + ROM API） */
int boot_flash_erase(uint32_t address);

/** @brief 编程（关全局中断 + ROM API；长度任意，尾部按字补齐 0xFF） */
int boot_flash_program(uint32_t address, const void *buf, size_t len);

/** @brief DFU 写入器：开始一次镜像写入（base = 槽基址） */
void boot_writer_begin(uint32_t base);

/** @brief 顺序写入镜像数据（内部按 4KB 扇区缓冲，擦写时关中断） */
int boot_writer_write(uint32_t address, const void *data, size_t len);

/** @brief 提交最后一个未满扇区 */
int boot_writer_finish(void);

/** @brief 放弃本次写入（不提交未满扇区；已提交的扇区保持） */
void boot_writer_abort(void);

#endif /* BOOT_FLASH_H */
