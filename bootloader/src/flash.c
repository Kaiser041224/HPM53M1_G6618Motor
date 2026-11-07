/**
 * @file    flash.c
 * @brief   Bootloader XPI NOR 访问 + DFU 扇区缓冲写入器
 * @author  Kaiser
 *
 * 纪律（与主工程 drv_flash.c 一致）：
 *   - 擦/写期间关闭全局中断（XPI 操作期间 flash 取指窗口不可用）；
 *   - ROM API 的源缓冲由 CPU 读取（非 DMA），D-Cache 一致性天然成立；
 *   - 擦/写后按 32B cache line 对齐 invalidate，保证后续 XIP 读不命中旧行。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "flash.h"

#include "board.h"
#include "hpm_common.h"
#include "hpm_csr_drv.h"
#include "hpm_l1c_drv.h"
#include "hpm_romapi.h"
#include "hpm_romapi_xpi_def.h"
#include "hpm_romapi_xpi_nor_def.h"
#include "hpm_soc.h"

#include <string.h>

#define SECTOR_SIZE (0x1000U)

static xpi_nor_config_t s_nor_cfg;
static uint32_t s_sector_size;

/* ---- DFU writer（4KB 扇区缓冲） ---- */
static uint32_t s_writer_base;
static uint32_t s_writer_sector;
static uint32_t s_writer_buffered;
static bool s_writer_has_sector;
static uint32_t s_writer_buf[SECTOR_SIZE / sizeof(uint32_t)];

static inline uint32_t flash_enter_critical(void) {
    return read_clear_csr(CSR_MSTATUS, CSR_MSTATUS_MIE_MASK);
}

static inline void flash_exit_critical(uint32_t state) {
    write_csr(CSR_MSTATUS, state);
}

static void cache_invalidate(uint32_t address, size_t size) {
    uint32_t start = HPM_L1C_CACHELINE_ALIGN_DOWN(address);
    uint32_t end = HPM_L1C_CACHELINE_ALIGN_UP(address + (uint32_t) size);

    l1c_dc_invalidate(start, end - start);
}

void boot_flash_init(void) {
    xpi_nor_config_option_t option;

    option.header.U = BOARD_APP_XPI_NOR_CFG_OPT_HDR;
    option.option0.U = BOARD_APP_XPI_NOR_CFG_OPT_OPT0;
    option.option1.U = BOARD_APP_XPI_NOR_CFG_OPT_OPT1;
    (void) rom_xpi_nor_auto_config(BOARD_APP_XPI_NOR_XPI_BASE, &s_nor_cfg, &option);
    (void) rom_xpi_nor_get_property(BOARD_APP_XPI_NOR_XPI_BASE, &s_nor_cfg,
                                    xpi_nor_property_sector_size, &s_sector_size);
}

const boot_flash_ops_t *boot_flash_ops(void) {
    static const boot_flash_ops_t ops = {
        .read = boot_flash_read,
        .erase_sector = boot_flash_erase,
        .program = boot_flash_program,
    };

    return &ops;
}

int boot_flash_read(uint32_t address, void *buf, size_t len) {
    if ((buf == NULL) || ((uint64_t) address + len > (uint64_t) (BOOT_FLASH_BASE + BOOT_FLASH_SIZE))) {
        return -1;
    }
    memcpy(buf, (const void *) address, len);
    return 0;
}

int boot_flash_erase(uint32_t address) {
    uint32_t flags;
    hpm_stat_t status;

    if ((s_sector_size != SECTOR_SIZE) || ((address % SECTOR_SIZE) != 0U)
        || (address < BOOT_FLASH_BASE)
        || (address >= (BOOT_FLASH_BASE + BOOT_FLASH_SIZE))) {
        return -1;
    }

    flags = flash_enter_critical();
    status = rom_xpi_nor_erase_sector(BOARD_APP_XPI_NOR_XPI_BASE, xpi_xfer_channel_auto,
                                      &s_nor_cfg, address - BOOT_FLASH_BASE);
    flash_exit_critical(flags);

    if (status != status_success) {
        return -1;
    }
    cache_invalidate(address, SECTOR_SIZE);
    return 0;
}

int boot_flash_program(uint32_t address, const void *buf, size_t len) {
    const uint8_t *src = (const uint8_t *) buf;
    uint32_t flags;
    hpm_stat_t status;
    size_t words;
    size_t tail;

    if ((buf == NULL) || (len == 0U) || ((address % 4U) != 0U)
        || ((uint64_t) address + len > (uint64_t) (BOOT_FLASH_BASE + BOOT_FLASH_SIZE))) {
        return -1;
    }

    words = len / sizeof(uint32_t);
    tail = len - (words * sizeof(uint32_t));

    if (words > 0U) {
        flags = flash_enter_critical();
        status = rom_xpi_nor_program(BOARD_APP_XPI_NOR_XPI_BASE, xpi_xfer_channel_auto, &s_nor_cfg,
                                     (const uint32_t *) src, address - BOOT_FLASH_BASE,
                                     (uint32_t) (words * sizeof(uint32_t)));
        flash_exit_critical(flags);
        if (status != status_success) {
            return -1;
        }
    }

    if (tail > 0U) {
        uint32_t tail_word = 0xFFFFFFFFUL;

        memcpy(&tail_word, src + (words * sizeof(uint32_t)), tail);
        flags = flash_enter_critical();
        status = rom_xpi_nor_program(BOARD_APP_XPI_NOR_XPI_BASE, xpi_xfer_channel_auto, &s_nor_cfg,
                                     &tail_word,
                                     (address - BOOT_FLASH_BASE) + (words * sizeof(uint32_t)),
                                     sizeof(tail_word));
        flash_exit_critical(flags);
        if (status != status_success) {
            return -1;
        }
    }

    cache_invalidate(address, len);
    return 0;
}

/* ============================================================================
 * DFU writer
 * ============================================================================ */

void boot_writer_begin(uint32_t base) {
    s_writer_base = base;
    s_writer_sector = 0U;
    s_writer_buffered = 0U;
    s_writer_has_sector = false;
}

static int writer_commit(void) {
    const uint8_t *flash = (const uint8_t *) s_writer_sector;

    if (!s_writer_has_sector || (s_writer_buffered == 0U)) {
        return 0;
    }
    if (memcmp(flash, s_writer_buf, s_writer_buffered) == 0) {
        return 0;
    }
    if (boot_flash_erase(s_writer_sector) != 0) {
        return -1;
    }
    return boot_flash_program(s_writer_sector, s_writer_buf, s_writer_buffered);
}

int boot_writer_write(uint32_t address, const void *data, size_t len) {
    const uint8_t *src = (const uint8_t *) data;
    size_t offset = 0U;

    if ((data == NULL) || (len == 0U) || (address < s_writer_base)
        || ((uint64_t) address + len > (uint64_t) (s_writer_base + BOOT_SLOT_SIZE))) {
        return -1;
    }

    while (offset < len) {
        uint32_t sector = (address + (uint32_t) offset) & ~(SECTOR_SIZE - 1U);
        uint32_t sector_off = (address + (uint32_t) offset) - sector;
        size_t chunk = SECTOR_SIZE - sector_off;

        if (chunk > (len - offset)) {
            chunk = len - offset;
        }

        if (!s_writer_has_sector || (s_writer_sector != sector)) {
            if (writer_commit() != 0) {
                return -1;
            }
            s_writer_sector = sector;
            s_writer_buffered = 0U;
            s_writer_has_sector = true;
        }

        memcpy((uint8_t *) s_writer_buf + sector_off, src + offset, chunk);
        if ((sector_off + chunk) > s_writer_buffered) {
            s_writer_buffered = sector_off + (uint32_t) chunk;
        }
        offset += chunk;

        if ((sector_off + chunk) == SECTOR_SIZE) {
            if (writer_commit() != 0) {
                return -1;
            }
            s_writer_has_sector = false;
            s_writer_buffered = 0U;
        }
    }
    return 0;
}

int boot_writer_finish(void) {
    int rc = writer_commit();

    s_writer_has_sector = false;
    s_writer_buffered = 0U;
    return rc;
}

void boot_writer_abort(void) {
    s_writer_has_sector = false;
    s_writer_buffered = 0U;
}
