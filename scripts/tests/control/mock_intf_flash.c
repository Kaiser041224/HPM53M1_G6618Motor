/**
 * @file    mock_intf_flash.c
 * @brief   Host test flash device for app_cogging_store
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_flash.h"

#include <string.h>

#define MOCK_SECTOR_SIZE (4096U)
#define MOCK_SECTOR_COUNT (3U)
#define MOCK_FLASH_BASE   (0x80000000U)
#define MOCK_PAGE_SIZE    (256U)

static uint8_t s_data[MOCK_SECTOR_SIZE * MOCK_SECTOR_COUNT];

static int mock_init(void) { return 0; }
static uint32_t mock_base(void) { return MOCK_FLASH_BASE; }
static uint32_t mock_size(void) { return sizeof(s_data); }
static uint32_t mock_sector_size(void) { return MOCK_SECTOR_SIZE; }

static int mock_read(uint32_t addr, void* buf, size_t len) {
    if (buf == NULL || addr < MOCK_FLASH_BASE
        || (addr - MOCK_FLASH_BASE) + len > sizeof(s_data)) return -1;
    memcpy(buf, &s_data[addr - MOCK_FLASH_BASE], len);
    return 0;
}

static int mock_program(uint32_t addr, const void* buf, size_t len) {
    if (buf == NULL || addr < MOCK_FLASH_BASE
        || (addr - MOCK_FLASH_BASE) + len > sizeof(s_data)) return -1;
    /* 模拟 NOR page program：一次调用不得跨 256B page。 */
    if (((addr - MOCK_FLASH_BASE) % MOCK_PAGE_SIZE) + len > MOCK_PAGE_SIZE) return -1;
    memcpy(&s_data[addr - MOCK_FLASH_BASE], buf, len);
    return 0;
}

static int mock_erase(uint32_t addr) {
    if (addr < MOCK_FLASH_BASE
        || (addr - MOCK_FLASH_BASE) + MOCK_SECTOR_SIZE > sizeof(s_data)) return -1;
    memset(&s_data[addr - MOCK_FLASH_BASE], 0xFF, MOCK_SECTOR_SIZE);
    return 0;
}

void mock_intf_flash_reset(void) { memset(s_data, 0xFF, sizeof(s_data)); }

uint8_t* mock_intf_flash_data(void) { return s_data; }

const intf_flash_t* intf_flash_get(void) {
    static const intf_flash_t flash = {
        .instance_id = 0,
        .init = mock_init,
        .get_base_addr = mock_base,
        .get_size = mock_size,
        .get_sector_size = mock_sector_size,
        .read = mock_read,
        .program = mock_program,
        .erase_sector = mock_erase,
    };
    return &flash;
}
