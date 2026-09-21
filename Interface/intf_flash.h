/**
 * @file    intf_flash.h
 * @brief   Flash 抽象接口（设备对象 + 匿名结构体）
 * @author  Kaiser
 *
 * 语义约定：
 *   - 地址为 CPU 视角的绝对地址（XIP 空间，本板 0x80000000 起）
 *   - program：目标区间必须已擦除（0xFF），且不跨扇区（调用方保证）；
 *     src 缓冲区需 4 字节对齐
 *   - erase_sector：addr 必须扇区对齐
 *   - 擦/写期间关闭全局中断（flash 读窗口不可用），单次耗时 ms 级
 *   - 本 SoC 单实例，instance_id 固定为 0
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef INTF_FLASH_H
#define INTF_FLASH_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Flash 设备对象
 */
typedef struct {
    uint8_t instance_id; /**< 实例编号（单实例，= 0） */
    struct {
        /**
         * @brief 初始化 Flash 驱动
         * @return 0 = 成功；-1 = 失败
         */
        int (*init)(void);

        /**
         * @brief 读取 XIP 基址
         * @return 基址（0x80000000）
         */
        uint32_t (*get_base_addr)(void);

        /**
         * @brief 读取总容量
         * @return 总容量 [字节]
         */
        uint32_t (*get_size)(void);

        /**
         * @brief 读取擦除扇区大小
         * @return 扇区大小 [字节]
         */
        uint32_t (*get_sector_size)(void);

        /**
         * @brief 读取数据
         * @param addr 起始地址（XIP 空间）
         * @param buf 输出缓冲区
         * @param len 读取长度 [字节]
         * @return 0 = 成功；-1 = 失败
         */
        int (*read)(uint32_t addr, void *buf, size_t len);

        /**
         * @brief 写入数据
         * @param addr 起始地址（须已擦除且不跨扇区）
         * @param buf 源缓冲区（需 4 字节对齐）
         * @param len 写入长度 [字节]
         * @return 0 = 成功；-1 = 失败
         */
        int (*program)(uint32_t addr, const void *buf, size_t len);

        /**
         * @brief 擦除一个扇区
         * @param addr 扇区对齐地址
         * @return 0 = 成功；-1 = 失败
         */
        int (*erase_sector)(uint32_t addr);
    };
} intf_flash_t;

/**
 * @brief 注册 Flash 设备对象
 * @param dev 设备对象
 * @return 0 = 成功；-1 = 失败
 */
int intf_flash_register(const intf_flash_t *dev);

/**
 * @brief 获取 Flash 设备对象
 * @return 设备对象指针；NULL = 未注册
 */
const intf_flash_t *intf_flash_get(void);

#ifdef __cplusplus
}
#endif

#endif /* INTF_FLASH_H */
