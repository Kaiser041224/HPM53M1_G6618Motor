/**
 * @file    boot_image.c
 * @brief   active/backup 槽镜像头占位（仅槽构建）
 * @author  Kaiser
 *
 * 槽链接脚本把 .image_header 放在槽基址，入口固定为 基址 + 0x40。
 * 链接完成后由 scripts/gen_image_header.py 在 .bin 中写入：
 *   signature / magic / slot_id / image_version / image_size / image_crc32 / ...
 * 因此本文件的内容始终是全 0，仅用于在镜像中占住 64B。
 *
 * 非槽构建（未定义 APP_SLOT_ID）编译为空 TU。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef APP_SLOT_ID

#include "intf_boot.h"

__attribute__((section(".image_header"), used)) const boot_image_header_t g_boot_image_header = {0};

#endif /* APP_SLOT_ID */
