/*
 * Flash Driver - HPM XPI NOR 适配（boot ROM API）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
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
 */

#include "intf_flash.h"
#include "board.h"

#include "hpm_romapi.h"
#include "hpm_csr_drv.h"

static xpi_nor_config_t s_nor_cfg;
static uint32_t s_base;
static uint32_t s_size;
static uint32_t s_sector_size;
static bool s_initialized;

/* 擦/写临界区：保存并关闭全局中断（flash 操作期间读窗口不可用） */
static inline uint32_t flash_enter_critical(void)
{
    return read_clear_csr(CSR_MSTATUS, CSR_MSTATUS_MIE_MASK);
}

static inline void flash_exit_critical(uint32_t irq_state)
{
    write_csr(CSR_MSTATUS, irq_state);
}

static bool flash_addr_in_range(uint32_t addr, uint32_t len)
{
    return (addr >= s_base) && ((addr + len) <= (s_base + s_size)) && (len > 0U);
}

/* ============================================================================
 * 实现（单实例）
 * ============================================================================ */

static int flash_init_impl(void)
{
    xpi_nor_config_option_t option;

    if (s_initialized) {
        return 0; /* 幂等：auto_config 仅执行一次 */
    }

    option.header.U = BOARD_APP_XPI_NOR_CFG_OPT_HDR;
    option.option0.U = BOARD_APP_XPI_NOR_CFG_OPT_OPT0;
    option.option1.U = BOARD_APP_XPI_NOR_CFG_OPT_OPT1;

    if (rom_xpi_nor_auto_config(BOARD_APP_XPI_NOR_XPI_BASE, &s_nor_cfg, &option) != status_success) {
        return -1;
    }

    s_base = BOARD_APP_XPI_NOR_BASE_ADDR;

    if (rom_xpi_nor_get_property(BOARD_APP_XPI_NOR_XPI_BASE, &s_nor_cfg,
                                 xpi_nor_property_total_size, &s_size) != status_success) {
        return -1;
    }
    if (rom_xpi_nor_get_property(BOARD_APP_XPI_NOR_XPI_BASE, &s_nor_cfg,
                                 xpi_nor_property_sector_size, &s_sector_size) != status_success) {
        return -1;
    }
    if ((s_size == 0U) || (s_sector_size == 0U)) {
        return -1;
    }

    s_initialized = true;
    return 0;
}

static uint32_t flash_get_base_addr_impl(void)
{
    return s_base;
}

static uint32_t flash_get_size_impl(void)
{
    return s_size;
}

static uint32_t flash_get_sector_size_impl(void)
{
    return s_sector_size;
}

static int flash_read_impl(uint32_t addr, void *buf, size_t len)
{
    if (!s_initialized || (buf == NULL) || !flash_addr_in_range(addr, (uint32_t) len)) {
        return -1;
    }

    return (rom_xpi_nor_read(BOARD_APP_XPI_NOR_XPI_BASE, xpi_xfer_channel_auto, &s_nor_cfg,
                             (uint32_t *) buf, addr - s_base, (uint32_t) len) == status_success)
               ? 0
               : -1;
}

static int flash_program_impl(uint32_t addr, const void *buf, size_t len)
{
    uint32_t irq_state;
    hpm_stat_t st;

    if (!s_initialized || (buf == NULL) || !flash_addr_in_range(addr, (uint32_t) len)) {
        return -1;
    }
    /* 不跨扇区（ROM API 单次调用约定） */
    if (((addr - s_base) % s_sector_size) + len > s_sector_size) {
        return -1;
    }

    irq_state = flash_enter_critical();
    st = rom_xpi_nor_program(BOARD_APP_XPI_NOR_XPI_BASE, xpi_xfer_channel_auto, &s_nor_cfg,
                             (const uint32_t *) buf, addr - s_base, (uint32_t) len);
    flash_exit_critical(irq_state);

    return (st == status_success) ? 0 : -1;
}

static int flash_erase_sector_impl(uint32_t addr)
{
    uint32_t irq_state;
    hpm_stat_t st;

    if (!s_initialized || !flash_addr_in_range(addr, 1U)) {
        return -1;
    }
    if (((addr - s_base) % s_sector_size) != 0U) {
        return -1;
    }

    irq_state = flash_enter_critical();
    st = rom_xpi_nor_erase_sector(BOARD_APP_XPI_NOR_XPI_BASE, xpi_xfer_channel_auto, &s_nor_cfg,
                                  addr - s_base);
    flash_exit_critical(irq_state);

    return (st == status_success) ? 0 : -1;
}

/* ============================================================================
 * 单实例设备对象（风格 A）
 * ============================================================================ */

static const intf_flash_t flash_dev = {
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
    intf_flash_register(&flash_dev);
}
