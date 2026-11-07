/**
 * @file    test_boot_meta.c
 * @brief   boot_meta.c 主机自测：CRC32 / 镜像头 / 追加日志 / 启动确认
 * @author  Kaiser
 *
 * 用 1MB NOR 语义 mock（program 只能把 1 变 0、erase 恢复 0xFF），
 * 覆盖断电追加写、扇区写满回卷、半写记录跳过、确认幂等。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_boot.h"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_fails;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_fails++;                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
        }                                                                                          \
    } while (0)

/* ============================================================================
 * NOR mock
 * ============================================================================ */
static uint8_t g_flash[BOOT_FLASH_SIZE];
static unsigned g_program_calls;
static unsigned g_erase_calls;

static int mock_read(uint32_t addr, void *buf, size_t len) {
    if ((buf == NULL) || (addr < BOOT_FLASH_BASE)
        || ((uint64_t) addr + len > (uint64_t) (BOOT_FLASH_BASE + BOOT_FLASH_SIZE))) {
        return -1;
    }
    memcpy(buf, &g_flash[addr - BOOT_FLASH_BASE], len);
    return 0;
}

static int mock_erase(uint32_t addr) {
    if (((addr % BOOT_META_SECTOR_SIZE) != 0U) || (addr < BOOT_FLASH_BASE)
        || (addr >= (BOOT_FLASH_BASE + BOOT_FLASH_SIZE))) {
        return -1;
    }
    g_erase_calls++;
    memset(&g_flash[addr - BOOT_FLASH_BASE], 0xFF, BOOT_META_SECTOR_SIZE);
    return 0;
}

static int mock_program(uint32_t addr, const void *buf, size_t len) {
    const uint8_t *src = (const uint8_t *) buf;

    if ((buf == NULL) || (addr < BOOT_FLASH_BASE)
        || ((uint64_t) addr + len > (uint64_t) (BOOT_FLASH_BASE + BOOT_FLASH_SIZE))) {
        return -1;
    }
    g_program_calls++;
    for (size_t i = 0U; i < len; i++) {
        g_flash[(addr - BOOT_FLASH_BASE) + i] &= src[i];
    }
    return 0;
}

static const boot_flash_ops_t g_ops = {
    .read = mock_read,
    .erase_sector = mock_erase,
    .program = mock_program,
};

static void flash_reset(void) {
    memset(g_flash, 0xFF, sizeof(g_flash));
    g_program_calls = 0U;
    g_erase_calls = 0U;
}

/* ============================================================================
 * 用例
 * ============================================================================ */
static void test_crc32(void) {
    CHECK(boot_crc32("123456789", 9U) == 0xCBF43926UL);
    CHECK(boot_crc32("", 0U) == 0U);
    CHECK(boot_crc32("a", 1U) == 0xE8B7BE43UL);
}

static boot_image_header_t make_header(uint8_t slot, uint32_t size, uint32_t version) {
    boot_image_header_t hdr;

    memset(&hdr, 0, sizeof(hdr));
    hdr.signature = BOOT_IMAGE_SIGNATURE;
    hdr.magic = BOOT_IMAGE_MAGIC;
    hdr.header_version = BOOT_IMAGE_HEADER_VERSION;
    hdr.slot_id = slot;
    hdr.image_version = version;
    hdr.image_size = size;
    hdr.image_crc32 = 0x12345678U;
    hdr.load_addr = boot_slot_address(slot);
    hdr.entry_offset = BOOT_IMAGE_ENTRY_OFFSET;
    hdr.header_crc32 = boot_crc32(&hdr, BOOT_IMAGE_HEADER_CRC_LEN);
    return hdr;
}

static void test_image_header(void) {
    boot_image_header_t hdr = make_header(0U, 1024U, 7U);

    CHECK(boot_image_header_valid(&hdr, 0U));
    CHECK(!boot_image_header_valid(&hdr, 1U)); /* 槽号不匹配 */

    hdr.slot_id = 1U;
    hdr.load_addr = boot_slot_address(1U);
    hdr.header_crc32 = boot_crc32(&hdr, BOOT_IMAGE_HEADER_CRC_LEN);
    CHECK(boot_image_header_valid(&hdr, 1U));
    CHECK(!boot_image_header_valid(&hdr, 0U));

    hdr.magic = 0U;
    CHECK(!boot_image_header_valid(&hdr, 1U)); /* 头字段被改但 CRC 未更新 */

    hdr = make_header(0U, 0U, 1U);
    CHECK(!boot_image_header_valid(&hdr, 0U)); /* size=0 */

    hdr = make_header(0U, BOOT_SLOT_SIZE - BOOT_IMAGE_HEADER_SIZE + 1U, 1U);
    CHECK(!boot_image_header_valid(&hdr, 0U)); /* 超槽容量 */

    hdr = make_header(0U, 1024U, 1U);
    hdr.entry_offset = 0x20U;
    hdr.header_crc32 = boot_crc32(&hdr, BOOT_IMAGE_HEADER_CRC_LEN);
    CHECK(!boot_image_header_valid(&hdr, 0U)); /* 入口固定 0x40 */

    CHECK(!boot_image_header_valid(NULL, 0U));
    CHECK(!boot_image_header_valid(&hdr, 2U)); /* 非法槽号 */
}

static void test_control_log(void) {
    boot_control_record_t rec;
    boot_control_record_t loaded;

    flash_reset();
    CHECK(boot_control_load(&g_ops, &loaded) == -1); /* 空扇区 */

    boot_control_defaults(&rec);
    CHECK(rec.active == 0U);
    CHECK(rec.pending == BOOT_SLOT_NONE);

    CHECK(boot_control_save(&g_ops, &rec) == 0);
    CHECK(boot_control_load(&g_ops, &loaded) == 0);
    CHECK(loaded.active == 0U);
    CHECK(loaded.pending == BOOT_SLOT_NONE);
    CHECK(loaded.seq == 0U);

    rec.pending = 1U;
    rec.attempts = 2U;
    CHECK(boot_control_save(&g_ops, &rec) == 0);
    CHECK(boot_control_load(&g_ops, &loaded) == 0);
    CHECK(loaded.pending == 1U);
    CHECK(loaded.attempts == 2U);
    CHECK(loaded.seq == 1U); /* seq 单调 */

    /* 半写记录：下一个槽位塞 0x00（magic/CRC 都坏）→ 必须被跳过 */
    {
        uint32_t off = 2U * sizeof(boot_control_record_t);
        uint8_t junk[sizeof(boot_control_record_t)];

        memset(junk, 0, sizeof(junk));
        CHECK(mock_program(BOOT_CONTROL_ADDR + off, junk, sizeof(junk)) == 0);
    }
    CHECK(boot_control_load(&g_ops, &loaded) == 0);
    CHECK(loaded.seq == 1U);

    /* 写满一个扇区：第 256 条后回卷（擦除一次），seq 继续递增 */
    for (uint32_t i = 0U; i < 300U; i++) {
        rec.attempts = (uint8_t) i;
        CHECK(boot_control_save(&g_ops, &rec) == 0);
    }
    CHECK(g_erase_calls == 1U);
    CHECK(boot_control_load(&g_ops, &loaded) == 0);
    CHECK(loaded.attempts == (uint8_t) 299U);
    CHECK(loaded.seq == 301U); /* 初始 2 条 + 300 条，中途回卷一次 */
}

static void test_image_record(void) {
    boot_image_record_t rec;
    uint32_t meta = boot_slot_meta_address(0U);

    flash_reset();
    CHECK(boot_image_record_load(&g_ops, meta, &rec) == -1);

    CHECK(boot_image_record_begin(&g_ops, meta, 4096U, 3U) == 0);
    CHECK(boot_image_record_load(&g_ops, meta, &rec) == 0);
    CHECK(rec.state == BOOT_IMAGE_STATE_FLASHING);
    CHECK(rec.image_size == 4096U);
    CHECK(rec.image_version == 3U);

    CHECK(boot_image_record_ready(&g_ops, meta, 4096U, 0xDEADBEEFU, 3U) == 0);
    CHECK(boot_image_record_load(&g_ops, meta, &rec) == 0);
    CHECK(rec.state == BOOT_IMAGE_STATE_READY);
    CHECK(rec.image_crc32 == 0xDEADBEEFU);

    /* 另一槽独立 */
    CHECK(boot_image_record_load(&g_ops, boot_slot_meta_address(1U), &rec) == -1);
}

static void test_confirm(void) {
    boot_control_record_t rec;
    boot_control_record_t loaded;
    unsigned programs;

    flash_reset();
    /* 首次启动：无记录 → 补一条 active=A/confirmed */
    CHECK(boot_confirm_running_slot(&g_ops, 0U) == 0);
    CHECK(boot_control_load(&g_ops, &loaded) == 0);
    CHECK(loaded.active == 0U);
    CHECK(loaded.pending == BOOT_SLOT_NONE);
    CHECK(loaded.confirmed == 1U);

    /* 幂等：已确认时不写 flash */
    programs = g_program_calls;
    CHECK(boot_confirm_running_slot(&g_ops, 0U) == 0);
    CHECK(g_program_calls == programs);

    /* pending=B 启动成功 → active 变 B */
    boot_control_defaults(&rec);
    rec.active = 0U;
    rec.pending = 1U;
    rec.attempts = 2U;
    CHECK(boot_control_save(&g_ops, &rec) == 0);
    CHECK(boot_confirm_running_slot(&g_ops, 1U) == 0);
    CHECK(boot_control_load(&g_ops, &loaded) == 0);
    CHECK(loaded.active == 1U);
    CHECK(loaded.pending == BOOT_SLOT_NONE);
    CHECK(loaded.attempts == 0U);
    CHECK(loaded.confirmed == 1U);

    CHECK(boot_confirm_running_slot(&g_ops, 2U) == -1); /* 非法槽号 */
}

int main(void) {
    test_crc32();
    test_image_header();
    test_control_log();
    test_image_record();
    test_confirm();

    printf("boot_meta host tests\n%d checks, %d failures\n", g_checks, g_fails);
    return (g_fails == 0) ? 0 : 1;
}
