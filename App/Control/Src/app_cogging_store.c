/**
 * @file    app_cogging_store.c
 * @brief   齿槽/传动角表独立 flash 扇区存储
 * @author  Kaiser
 *
 * flash 布局：倒数第 3 个 4KB 扇区（app_param 为倒数第 2，自检为最后 1）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_cogging_store.h"

#include "app_cold_ram.h"
#include "app_foc.h"
#include "intf_flash.h"

#include <stddef.h>
#include <string.h>

#define APP_COGGING_STORE_MAGIC   (0x47474F43U) /* "COGG" */
#define APP_COGGING_STORE_VERSION (1U)
#define APP_COGGING_STORE_PROGRAM_CHUNK (256U) /* NOR page program 单次上限 */

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t rotor_points;
    uint16_t output_points;
    uint16_t reserved;
    float rotor_scale_nm;
    float output_scale_nm;
    uint32_t crc32;
    int16_t rotor[ALGO_COGGING_ROTOR_POINTS];
    int16_t output[ALGO_COGGING_OUTPUT_POINTS];
    algo_cogging_stats_t rotor_stats;
    algo_cogging_stats_t output_stats;
    bool rotor_valid;
    bool output_valid;
} app_cogging_store_record_t;

/* 1.6KB 冷缓冲；不占 DLM，只在 FOC OFF 保存/加载时读写。 */
static app_cogging_store_record_t s_record APP_ATTR_AHB_SRAM;
static app_cogging_tables_t s_loaded_tables APP_ATTR_AHB_SRAM;

static uint32_t cogging_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0U; i < len; i++) {
        crc ^= data[i];
        for (uint32_t bit = 0U; bit < 8U; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1U)));
        }
    }
    return ~crc;
}

static int cogging_sector(const intf_flash_t** flash_out, uint32_t* address_out) {
    const intf_flash_t* flash = intf_flash_get();
    if ((flash == NULL) || (flash->init() != 0)) {
        return -1;
    }
    uint32_t base = flash->get_base_addr();
    uint32_t size = flash->get_size();
    uint32_t sector = flash->get_sector_size();
    if ((base == 0U) || (size < (3U * sector)) || (sector == 0U)) {
        return -1;
    }
    *flash_out = flash;
    *address_out = base + size - (3U * sector);
    return 0;
}

int app_cogging_store_load(void) {
    const intf_flash_t* flash = NULL;
    uint32_t address = 0U;

    if (cogging_sector(&flash, &address) != 0) {
        return -1;
    }
    if (flash->read(address, &s_record, sizeof(s_record)) != 0) {
        return -1;
    }
    if ((s_record.magic != APP_COGGING_STORE_MAGIC)
        || (s_record.version != APP_COGGING_STORE_VERSION)
        || (s_record.rotor_points != ALGO_COGGING_ROTOR_POINTS)
        || (s_record.output_points != ALGO_COGGING_OUTPUT_POINTS)
        || (s_record.rotor_scale_nm != APP_COGGING_ROTOR_SCALE_NM)
        || (s_record.output_scale_nm != APP_COGGING_OUTPUT_SCALE_NM)) {
        return -1;
    }
    uint32_t saved_crc = s_record.crc32;
    s_record.crc32 = 0U;
    uint32_t crc = cogging_crc32((const uint8_t*)&s_record, sizeof(s_record));
    s_record.crc32 = saved_crc;
    if (crc != saved_crc) {
        return -1;
    }

    memset(&s_loaded_tables, 0, sizeof(s_loaded_tables));
    memcpy(s_loaded_tables.rotor, s_record.rotor, sizeof(s_loaded_tables.rotor));
    memcpy(s_loaded_tables.output, s_record.output, sizeof(s_loaded_tables.output));
    s_loaded_tables.rotor_stats = s_record.rotor_stats;
    s_loaded_tables.output_stats = s_record.output_stats;
    s_loaded_tables.rotor_valid = s_record.rotor_valid;
    s_loaded_tables.output_valid = s_record.output_valid;
    return app_cogging_set_tables(&s_loaded_tables);
}

int app_cogging_store_save(void) {
    const intf_flash_t* flash = NULL;
    uint32_t address = 0U;
    const app_cogging_tables_t* tables = app_cogging_get_tables();

    if ((tables == NULL) || (!tables->rotor_valid && !tables->output_valid)
        || (app_foc_get_state() != APP_FOC_STATE_OFF) || app_foc_stop_pending()
        || (cogging_sector(&flash, &address) != 0)) {
        return -1;
    }

    memset(&s_record, 0, sizeof(s_record));
    s_record.magic = APP_COGGING_STORE_MAGIC;
    s_record.version = APP_COGGING_STORE_VERSION;
    s_record.rotor_points = ALGO_COGGING_ROTOR_POINTS;
    s_record.output_points = ALGO_COGGING_OUTPUT_POINTS;
    s_record.rotor_scale_nm = APP_COGGING_ROTOR_SCALE_NM;
    s_record.output_scale_nm = APP_COGGING_OUTPUT_SCALE_NM;
    memcpy(s_record.rotor, tables->rotor, sizeof(s_record.rotor));
    memcpy(s_record.output, tables->output, sizeof(s_record.output));
    s_record.rotor_stats = tables->rotor_stats;
    s_record.output_stats = tables->output_stats;
    s_record.rotor_valid = tables->rotor_valid;
    s_record.output_valid = tables->output_valid;
    s_record.crc32 = 0U;
    s_record.crc32 = cogging_crc32((const uint8_t*)&s_record, sizeof(s_record));

    if (flash->erase_sector(address) != 0) {
        return -1;
    }
    /* 与 app_param_store 一致：按 256B 槽/页提交，避免 ROM API 一次
     * 跨 page 写大块。整条记录一次 program 在本板曾导致 save 卡死。 */
    const uint8_t* src = (const uint8_t*)&s_record;
    for (uint32_t offset = 0U; offset < sizeof(s_record); offset += APP_COGGING_STORE_PROGRAM_CHUNK) {
        uint32_t remaining = (uint32_t)sizeof(s_record) - offset;
        uint32_t chunk = (remaining > APP_COGGING_STORE_PROGRAM_CHUNK)
                             ? APP_COGGING_STORE_PROGRAM_CHUNK
                             : remaining;
        if (flash->program(address + offset, src + offset, chunk) != 0) {
            return -1;
        }
    }
    /* 回读校验：防止 flash 编程静默失败。 */
    memset(&s_record, 0, sizeof(s_record));
    if (flash->read(address, &s_record, sizeof(s_record)) != 0) {
        return -1;
    }
    if (s_record.magic != APP_COGGING_STORE_MAGIC) {
        return -1;
    }
    uint32_t saved_crc = s_record.crc32;
    s_record.crc32 = 0U;
    uint32_t verify_crc = cogging_crc32((const uint8_t*)&s_record, sizeof(s_record));
    return (verify_crc == saved_crc) ? 0 : -1;
}

int app_cogging_store_clear(void) {
    const intf_flash_t* flash = NULL;
    uint32_t address = 0U;

    if ((app_foc_get_state() != APP_FOC_STATE_OFF) || app_foc_stop_pending()
        || (cogging_sector(&flash, &address) != 0)) {
        return -1;
    }
    int rc = flash->erase_sector(address);
    if (rc == 0) {
        app_cogging_clear_tables();
    }
    return rc;
}
