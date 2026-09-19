/*
 * Debug Flash - XPI NOR 自检
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 测试内容：
 *   1) 初始化（auto_config）+ 属性打印（基址 / 容量 / 扇区大小）
 *   2) 参数扇区（倒数第 2 扇区）只读预览（不擦写）
 *   3) 破坏性读写测试（仅最后 1 个扇区）：
 *      空白校验（全 0xFF）→ 擦除 → 再校验空白 → 写图案 → 回读比对
 *
 * 安全：
 *   - 绝不触碰固件区与参数扇区（测试地址 = 末尾扇区，且擦除前要求其为空白）
 *   - 擦/写由驱动关闭全局中断（ms 级），本测试仅在启动阶段执行一次
 */

#include "app_debug_flash.h"

#include "app_debug_rtt.h"
#include "intf_flash.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* 驱动注册（App 层不含 hpm_* 头文件，沿用既有 extern 约定） */
extern void hpm_flash_driver_register(void);

#define FLASH_TEST_BUF_SIZE (256U)
#define FLASH_ERASED_BYTE   (0xFFU)

/* 破坏性读写测试开关：bring-up 阶段为 1；量产固件应置 0（避免每次上电擦写） */
#define FLASH_RUN_DESTRUCTIVE_TEST (1)

/* 4 字节对齐：ROM API 的 src/dst 需对齐访问 */
static uint8_t s_wbuf[FLASH_TEST_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t s_rbuf[FLASH_TEST_BUF_SIZE] __attribute__((aligned(4)));

/* 测试图案（用于重复运行时识别测试区） */
static uint8_t flash_test_pattern(size_t i)
{
    return (uint8_t) (i ^ 0x5AU);
}

static bool flash_buf_is_erased(const uint8_t *buf, size_t len)
{
    for (size_t i = 0U; i < len; i++) {
        if (buf[i] != FLASH_ERASED_BYTE) {
            return false;
        }
    }
    return true;
}

/*
 * 安全判据：允许擦除的内容 = 空白（首次使用）或上次测试图案（重复运行）。
 * 出现其他内容（如固件数据）说明该扇区不属于保留区 → 拒绝擦除。
 */
static bool flash_sector_is_test_area(const uint8_t *buf, size_t len)
{
    for (size_t i = 0U; i < len; i++) {
        if ((buf[i] != FLASH_ERASED_BYTE) && (buf[i] != flash_test_pattern(i))) {
            return false;
        }
    }
    return true;
}

static bool flash_test_sector(const intf_flash_t *flash, uint32_t addr)
{
    /* 1) 擦除前安全检查：仅允许空白或上次测试图案（保护固件/参数区） */
    memset(s_rbuf, 0, sizeof(s_rbuf));
    if (flash->read(addr, s_rbuf, sizeof(s_rbuf)) != 0) {
        app_debug_printf("[FLASH] pre-read FAILED\r\n");
        return false;
    }
    if (!flash_sector_is_test_area(s_rbuf, sizeof(s_rbuf))) {
        app_debug_printf("[FLASH] sector not test area -> 疑似未预留，跳过测试\r\n");
        return false;
    }

    /* 2) 擦除 + 空白校验 */
    if (flash->erase_sector(addr) != 0) {
        app_debug_printf("[FLASH] erase FAILED\r\n");
        return false;
    }
    memset(s_rbuf, 0, sizeof(s_rbuf));
    if ((flash->read(addr, s_rbuf, sizeof(s_rbuf)) != 0) ||
        !flash_buf_is_erased(s_rbuf, sizeof(s_rbuf))) {
        app_debug_printf("[FLASH] blank check FAILED\r\n");
        return false;
    }

    /* 3) 写图案 + 回读比对 */
    for (uint32_t i = 0U; i < sizeof(s_wbuf); i++) {
        s_wbuf[i] = flash_test_pattern(i);
    }
    if (flash->program(addr, s_wbuf, sizeof(s_wbuf)) != 0) {
        app_debug_printf("[FLASH] program FAILED\r\n");
        return false;
    }

    memset(s_rbuf, 0, sizeof(s_rbuf));
    if (flash->read(addr, s_rbuf, sizeof(s_rbuf)) != 0) {
        app_debug_printf("[FLASH] verify read FAILED\r\n");
        return false;
    }
    if (memcmp(s_wbuf, s_rbuf, sizeof(s_wbuf)) != 0) {
        app_debug_printf("[FLASH] verify mismatch\r\n");
        return false;
    }

    return true;
}

void app_debug_flash_init(void)
{
    const intf_flash_t *flash;
    uint32_t base, size, sector, test_addr, param_addr;
    uint8_t head[16];

    app_debug_printf("\r\n[FLASH] XPI NOR self-test\r\n");

    hpm_flash_driver_register();
    flash = intf_flash_get();
    if ((flash == NULL) || (flash->init() != 0)) {
        app_debug_printf("[FLASH] init FAILED\r\n");
        return;
    }

    base = flash->get_base_addr();
    size = flash->get_size();
    sector = flash->get_sector_size();
    if ((base == 0U) || (size == 0U) || (sector == 0U)) {
        app_debug_printf("[FLASH] invalid properties\r\n");
        return;
    }
    app_debug_printf("[FLASH] base=0x%08X size=%u KB sector=%u B\r\n",
                     (unsigned) base, (unsigned) (size / 1024U), (unsigned) sector);

    test_addr = base + size - sector;         /* 最后 1 个扇区（测试用） */
    param_addr = base + size - (2U * sector); /* 倒数第 2 个扇区（参数区，只读预览） */

    /* 参数扇区只读预览（不擦写） */
    memset(head, 0, sizeof(head));
    if (flash->read(param_addr, head, sizeof(head)) == 0) {
        app_debug_printf("[FLASH] param sector @0x%08X head:", (unsigned) param_addr);
        for (uint32_t i = 0U; i < sizeof(head); i++) {
            app_debug_printf(" %02X", head[i]);
        }
        app_debug_printf("\r\n");
    }

    /* 破坏性读写测试（仅测试扇区） */
#if FLASH_RUN_DESTRUCTIVE_TEST
    app_debug_printf("[FLASH] test sector @0x%08X ...\r\n", (unsigned) test_addr);
    app_debug_printf("[FLASH] read/write test: %s\r\n",
                     flash_test_sector(flash, test_addr) ? "PASS" : "FAIL");
#else
    (void) test_addr;
    app_debug_printf("[FLASH] destructive test disabled\r\n");
#endif
}
