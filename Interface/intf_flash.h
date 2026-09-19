/*
 * Flash Interface - C17 抽象接口（设备对象 + 匿名结构体）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 语义约定：
 *   - 地址为 CPU 视角的绝对地址（XIP 空间，本板 0x80000000 起）
 *   - program：目标区间必须已擦除（0xFF），且不跨扇区（调用方保证）；
 *     src 缓冲区需 4 字节对齐
 *   - erase_sector：addr 必须扇区对齐
 *   - 擦/写期间关闭全局中断（flash 读窗口不可用），单次耗时 ms 级
 *   - 本 SoC 单实例，instance_id 固定为 0
 */

#ifndef INTF_FLASH_H
#define INTF_FLASH_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t instance_id; /* 单实例，= 0 */
    struct {
        int      (*init)(void);
        uint32_t (*get_base_addr)(void);   /* XIP 基址（0x80000000） */
        uint32_t (*get_size)(void);        /* 总容量（字节） */
        uint32_t (*get_sector_size)(void); /* 擦除扇区大小（字节） */
        int      (*read)(uint32_t addr, void *buf, size_t len);
        int      (*program)(uint32_t addr, const void *buf, size_t len);
        int      (*erase_sector)(uint32_t addr);
    };
} intf_flash_t;

int intf_flash_register(const intf_flash_t *dev);
const intf_flash_t *intf_flash_get(void);

#ifdef __cplusplus
}
#endif

#endif /* INTF_FLASH_H */
