/**
 * @file    dfu.c
 * @brief   DFU 下载/清单回调与写入会话（TinyUSB DFU 1.1）
 * @author  Kaiser
 *
 * 数据流：
 *   host --(EP0 control, 1024B/块)--> tud_dfu_download_cb
 *        -> boot_writer_write（4KB 扇区缓冲，跨扇区时擦+写）
 *   host --(zero-length DNLOAD)--> manifest
 *        -> 读回镜像头 → 校验槽号/长度/CRC32 → READY 记录 + pending → 1.2s 后复位
 *
 * 槽号保护：镜像头里的 slot_id/load_addr 必须等于 DFU 目标槽；发错文件直接
 * 返回 DFU_STATUS_ERR_FIRMWARE，旧镜像不受影响。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "dfu.h"

#include "boot.h"
#include "flash.h"

#include "board.h"
#include "hpm_clock_drv.h"
#include "hpm_csr_drv.h"
#include "hpm_soc.h"
#include "tusb.h"

#include <stddef.h>

/** manifest 成功后延时复位：给主机留出读取最后一次 GETSTATUS 的时间 */
#define DFU_RESET_DELAY_MS (1200U)

typedef struct {
    bool active;
    uint8_t slot;
    uint32_t base;
    uint32_t received;     /**< 已写入字节数（含 64B 头） */
    uint32_t expect_block; /**< 下一块的 block 号（必须连续） */
} dfu_session_t;

static dfu_session_t s_session;
static bool s_seen_traffic;
static uint32_t s_start_ms;
static uint32_t s_reset_at_ms;

/* ============================================================================
 * TinyUSB 时间基准（不链接 SDK 的 hw/bsp/hpmicro/family.c：其 board_millis
 * 使用 rdcycle(0xC00)，本工程统一改用 mcycle(0xB00)）
 * ============================================================================ */
uint32_t tusb_time_millis_api(void) {
    return (uint32_t) (hpm_csr_get_core_mcycle() / (hpm_core_clock / 1000U));
}

/* ============================================================================
 * 会话
 * ============================================================================ */
static void session_abort(void) {
    boot_writer_abort();
    s_session.active = false;
}

static void download_fail(uint8_t status) {
    session_abort();
    tud_dfu_finish_flashing(status);
}

/* ============================================================================
 * DFU 回调
 * ============================================================================ */
uint32_t tud_dfu_get_timeout_cb(uint8_t alt, uint8_t state) {
    (void) alt;
    if (state == DFU_DNBUSY) {
        return 100U; /* 单块写入上限：擦 4KB + 写 4KB */
    }
    if (state == DFU_MANIFEST) {
        /* manifest 自身工作 <200ms；不能超过复位延时（1.2s），否则主机
         * 睡醒后再读状态时设备已复位，dfu-util 会报 LIBUSB_ERROR_IO。 */
        return 500U;
    }
    return 0U;
}

void tud_dfu_download_cb(uint8_t alt, uint16_t block_num, uint8_t const *data, uint16_t length) {
    const boot_image_header_t *hdr;

    (void) alt;
    s_seen_traffic = true;

    /* 主机重试/重连会从块 0 重新开始：丢弃旧的未完成会话 */
    if (s_session.active && (block_num == 0U)) {
        session_abort();
    }

    if (!s_session.active) {
        if ((block_num != 0U) || (data == NULL) || (length < BOOT_IMAGE_HEADER_SIZE)) {
            download_fail(DFU_STATUS_ERR_FIRMWARE);
            return;
        }
        hdr = (const boot_image_header_t *) data;
        if (!boot_image_header_valid(hdr, s_session.slot)) {
            download_fail(DFU_STATUS_ERR_FIRMWARE);
            return;
        }

        s_session.base = boot_slot_address(s_session.slot);
        s_session.received = 0U;
        s_session.expect_block = 0U;
        boot_writer_begin(s_session.base);
        if (boot_slot_mark_flashing(s_session.slot, hdr) != 0) {
            download_fail(DFU_STATUS_ERR_WRITE);
            return;
        }
        s_session.active = true;
    }

    if (block_num != s_session.expect_block) {
        download_fail(DFU_STATUS_ERR_WRITE);
        return;
    }
    if (((uint64_t) s_session.received + length) > BOOT_SLOT_SIZE) {
        download_fail(DFU_STATUS_ERR_ADDRESS);
        return;
    }
    if (boot_writer_write(s_session.base + s_session.received, data, length) != 0) {
        download_fail(DFU_STATUS_ERR_WRITE);
        return;
    }

    s_session.received += length;
    s_session.expect_block++;
    tud_dfu_finish_flashing(DFU_STATUS_OK);
}

void tud_dfu_manifest_cb(uint8_t alt) {
    const boot_image_header_t *hdr;
    uint32_t crc;

    (void) alt;
    s_seen_traffic = true;

    if (!s_session.active) {
        tud_dfu_finish_flashing(DFU_STATUS_ERR_NOTDONE);
        return;
    }
    if (boot_writer_finish() != 0) {
        download_fail(DFU_STATUS_ERR_WRITE);
        return;
    }

    hdr = (const boot_image_header_t *) s_session.base;
    if (!boot_image_header_valid(hdr, s_session.slot)
        || (s_session.received != (BOOT_IMAGE_HEADER_SIZE + hdr->image_size))) {
        download_fail(DFU_STATUS_ERR_FIRMWARE);
        return;
    }

    crc = boot_crc32((const void *) (s_session.base + BOOT_IMAGE_HEADER_SIZE), hdr->image_size);
    if (crc != hdr->image_crc32) {
        download_fail(DFU_STATUS_ERR_VERIFY);
        return;
    }

    if (boot_slot_commit(s_session.slot, hdr, crc) != 0) {
        download_fail(DFU_STATUS_ERR_WRITE);
        return;
    }

    s_session.active = false;
    s_reset_at_ms = tusb_time_millis_api() + DFU_RESET_DELAY_MS;
    tud_dfu_finish_flashing(DFU_STATUS_OK);
}

void tud_dfu_detach_cb(void) {
}

void tud_dfu_abort_cb(uint8_t alt) {
    (void) alt;
    session_abort();
}

uint16_t tud_dfu_upload_cb(uint8_t alt, uint16_t block_num, uint8_t *data, uint16_t length) {
    (void) alt;
    (void) block_num;
    (void) data;
    (void) length;
    return 0U;
}

/* ============================================================================
 * USB ISR（TinyUSB HPM dcd 入口；不链接 SDK hw/bsp/hpmicro/family.c）
 * ============================================================================ */
SDK_DECLARE_EXT_ISR_M(IRQn_USB0, isr_usb0)
void isr_usb0(void) {
    tusb_int_handler(0, true);
}

/* ============================================================================
 * 入口
 * ============================================================================ */
uint8_t dfu_target_slot(void) {
    return s_session.slot;
}

void dfu_run(uint8_t target_slot, bool allow_timeout) {
    tusb_rhport_init_t init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_HIGH,
    };

    s_session.active = false;
    s_session.slot = target_slot;
    s_seen_traffic = false;
    s_reset_at_ms = 0U;

    board_init_usb();
    (void) tusb_init(0, &init);

    s_start_ms = tusb_time_millis_api();

    for (;;) {
        uint32_t now;

        tud_task();
        now = tusb_time_millis_api();

        if ((s_reset_at_ms != 0U) && ((int32_t) (now - s_reset_at_ms) >= 0)) {
            boot_system_reset();
        }
        if (allow_timeout && !s_seen_traffic
            && ((uint32_t) (now - s_start_ms) > BOOT_DFU_IDLE_TIMEOUT_MS)) {
            boot_system_reset();
        }
    }
}
