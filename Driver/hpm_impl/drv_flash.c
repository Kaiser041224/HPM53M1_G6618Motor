/**
 * @file    drv_flash.c
 * @brief   Flash 驱动 - HPM XPI NOR 适配（boot ROM API）
 * @author  Kaiser
 *
 * 实现策略：
 *   - 使用 boot ROM 的 XPI NOR API（rom_xpi_nor_*），与 SDK flashstress 样例一致
 *   - auto_config 依据板级 option（board.h）探测并配置 flash；容量/扇区大小运行时查询
 *   - 擦/写前关闭全局中断（mstatus.MIE 保存/恢复）：flash 读窗口在操作期间不可用
 *
 * 注意：
 *   - 本驱动会重新配置 XPI 控制器（auto_config），应在系统初始化阶段调用一次
 *   - 保留区（参数/测试扇区）由链接脚本在 flash 末尾预留，本驱动不感知，
 *     上层按 get_size()/get_sector_size() 计算地址
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_flash.h"
#include "board.h"

#include "hpm_romapi.h"
#include "hpm_csr_drv.h"

static xpi_nor_config_t s_nor_config;
static uint32_t s_base;
static uint32_t s_size;
static uint32_t s_sector_size;
static bool s_initialized;

/**
 * @brief 进入 flash 临界区（保存并关闭全局中断）
 * @return 保存的中断状态
 */
static inline uint32_t flash_enter_critical(void)
{
    return read_clear_csr(CSR_MSTATUS, CSR_MSTATUS_MIE_MASK);
}

/**
 * @brief 退出 flash 临界区（恢复全局中断）
 * @param irq_state 进入临界区时保存的中断状态
 */
static inline void flash_exit_critical(uint32_t irq_state)
{
    write_csr(CSR_MSTATUS, irq_state);
}

/**
 * @brief 判断地址区间是否落在 flash 容量内
 * @param addr 起始地址
 * @param len 长度 [byte]
 * @return true = 在范围内
 */
static bool flash_addr_in_range(uint32_t addr, uint32_t len)
{
    return (addr >= s_base) && ((addr + len) <= (s_base + s_size)) && (len > 0U);
}

/* ============================================================================
 * 实现（单实例）
 * ============================================================================ */

/**
 * @brief 初始化 XPI NOR（boot ROM auto_config，幂等）
 * @return 0 = 成功；-1 = 配置或容量查询失败
 */
static int flash_init_impl(void)
{
    xpi_nor_config_option_t option;

    if (s_initialized) {
        return 0; /* 幂等：auto_config 仅执行一次 */
    }

    option.header.U = BOARD_APP_XPI_NOR_CFG_OPT_HDR;
    option.option0.U = BOARD_APP_XPI_NOR_CFG_OPT_OPT0;
    option.option1.U = BOARD_APP_XPI_NOR_CFG_OPT_OPT1;

    if (rom_xpi_nor_auto_config(BOARD_APP_XPI_NOR_XPI_BASE, &s_nor_config, &option) != status_success) {
        return -1;
    }

    s_base = BOARD_APP_XPI_NOR_BASE_ADDR;

    if (rom_xpi_nor_get_property(BOARD_APP_XPI_NOR_XPI_BASE, &s_nor_config,
                                 xpi_nor_property_total_size, &s_size) != status_success) {
        return -1;
    }
    if (rom_xpi_nor_get_property(BOARD_APP_XPI_NOR_XPI_BASE, &s_nor_config,
                                 xpi_nor_property_sector_size, &s_sector_size) != status_success) {
        return -1;
    }
    if ((s_size == 0U) || (s_sector_size == 0U)) {
        return -1;
    }

    s_initialized = true;
    return 0;
}

/**
 * @brief 获取 flash 基地址
 * @return 基地址
 */
static uint32_t flash_get_base_addr_impl(void)
{
    return s_base;
}

/**
 * @brief 获取 flash 总容量
 * @return 容量 [byte]
 */
static uint32_t flash_get_size_impl(void)
{
    return s_size;
}

/**
 * @brief 获取 flash 扇区大小
 * @return 扇区大小 [byte]
 */
static uint32_t flash_get_sector_size_impl(void)
{
    return s_sector_size;
}

/**
 * @brief 从 flash 读取数据
 * @param addr 起始地址
 * @param buf 目的缓冲
 * @param len 长度 [byte]
 * @return 0 = 成功；-1 = 参数非法或 ROM 读失败
 */
static int flash_read_impl(uint32_t addr, void *buf, size_t len)
{
    if (!s_initialized || (buf == NULL) || !flash_addr_in_range(addr, (uint32_t) len)) {
        return -1;
    }

    return (rom_xpi_nor_read(BOARD_APP_XPI_NOR_XPI_BASE, xpi_xfer_channel_auto, &s_nor_config,
                             (uint32_t *) buf, addr - s_base, (uint32_t) len) == status_success)
               ? 0
               : -1;
}

/**
 * @brief 向 flash 编程数据（不跨扇区）
 * @param addr 起始地址
 * @param buf 源缓冲
 * @param len 长度 [byte]
 * @return 0 = 成功；-1 = 参数非法或 ROM 编程失败
 */
static int flash_program_impl(uint32_t addr, const void *buf, size_t len)
{
    uint32_t irq_state;
    hpm_stat_t status;

    if (!s_initialized || (buf == NULL) || !flash_addr_in_range(addr, (uint32_t) len)) {
        return -1;
    }
    /* 不跨扇区（ROM API 单次调用约定） */
    if (((addr - s_base) % s_sector_size) + len > s_sector_size) {
        return -1;
    }

    irq_state = flash_enter_critical();
    status = rom_xpi_nor_program(BOARD_APP_XPI_NOR_XPI_BASE, xpi_xfer_channel_auto, &s_nor_config,
                                 (const uint32_t *) buf, addr - s_base, (uint32_t) len);
    flash_exit_critical(irq_state);

    return (status == status_success) ? 0 : -1;
}

/**
 * @brief 擦除 flash 扇区
 * @param addr 扇区地址（须扇区对齐）
 * @return 0 = 成功；-1 = 参数非法或 ROM 擦除失败
 */
static int flash_erase_sector_impl(uint32_t addr)
{
    uint32_t irq_state;
    hpm_stat_t status;

    if (!s_initialized || !flash_addr_in_range(addr, 1U)) {
        return -1;
    }
    if (((addr - s_base) % s_sector_size) != 0U) {
        return -1;
    }

    irq_state = flash_enter_critical();
    status = rom_xpi_nor_erase_sector(BOARD_APP_XPI_NOR_XPI_BASE, xpi_xfer_channel_auto, &s_nor_config,
                                      addr - s_base);
    flash_exit_critical(irq_state);

    return (status == status_success) ? 0 : -1;
}

/* ============================================================================
 * 单实例设备对象（风格 A）
 * ============================================================================ */

static const intf_flash_t s_flash_dev = {
    .instance_id = 0U,
    .init = flash_init_impl,
    .get_base_addr = flash_get_base_addr_impl,
    .get_size = flash_get_size_impl,
    .get_sector_size = flash_get_sector_size_impl,
    .read = flash_read_impl,
    .program = flash_program_impl,
    .erase_sector = flash_erase_sector_impl,
};

void hpm_flash_driver_register(void)
{
    intf_flash_register(&s_flash_dev);
}
