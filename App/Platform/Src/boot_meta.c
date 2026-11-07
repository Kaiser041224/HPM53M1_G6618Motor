/**
 * @file    boot_meta.c
 * @brief   Bootloader / 应用共享的升级元数据、启动邮箱与软复位实现
 * @author  Kaiser
 *
 * 日志语义：
 *   - 记录定长（镜像 32B / 控制 16B），从扇区起始顺序追加；
 *   - 记录自带 CRC；扫描时跳过损坏/半写记录，遇到全 0xFF 记录即到末尾；
 *   - 扇区写满后擦除重来；写满周期远大于升级频率（控制日志 256 条）。
 *
 * 该文件同时链接进 bootloader 与应用：
 *   - bootloader 只使用 boot_control_* / boot_image_record_*；
 *   - 应用使用 boot_confirm_running_slot() 与邮箱接口。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_boot.h"

#include <string.h>

/* ============================================================================
 * CRC32（IEEE 802.3，反射）
 * ============================================================================ */
uint32_t boot_crc32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *) data;
    uint32_t crc = 0xFFFFFFFFUL;

    for (size_t i = 0U; i < len; i++) {
        crc ^= p[i];
        for (uint8_t bit = 0U; bit < 8U; bit++) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ 0xEDB88320UL;
            } else {
                crc >>= 1U;
            }
        }
    }
    return ~crc;
}

/* ============================================================================
 * 槽地址
 * ============================================================================ */
uint32_t boot_slot_address(uint8_t slot_id) {
    return (slot_id == 0U) ? BOOT_SLOT_ACTIVE_ADDR : BOOT_SLOT_BACKUP_ADDR;
}

uint32_t boot_slot_meta_address(uint8_t slot_id) {
    return (slot_id == 0U) ? BOOT_META_ACTIVE_ADDR : BOOT_META_BACKUP_ADDR;
}

/* ============================================================================
 * 镜像头
 * ============================================================================ */
bool boot_image_header_valid(const boot_image_header_t *hdr, uint8_t slot_id) {
    if (hdr == NULL) {
        return false;
    }
    if (slot_id >= BOOT_SLOT_COUNT) {
        return false;
    }
    if ((hdr->signature != BOOT_IMAGE_SIGNATURE) || (hdr->magic != BOOT_IMAGE_MAGIC)
        || (hdr->header_version != BOOT_IMAGE_HEADER_VERSION) || (hdr->slot_id != slot_id)
        || (hdr->entry_offset != BOOT_IMAGE_ENTRY_OFFSET)) {
        return false;
    }
    if (hdr->load_addr != boot_slot_address(slot_id)) {
        return false;
    }
    if ((hdr->image_size == 0U) || (hdr->image_size > (BOOT_SLOT_SIZE - BOOT_IMAGE_HEADER_SIZE))) {
        return false;
    }
    return boot_crc32(hdr, BOOT_IMAGE_HEADER_CRC_LEN) == hdr->header_crc32;
}

/* ============================================================================
 * 日志通用
 * ============================================================================ */
static bool record_is_erased(const boot_flash_ops_t *ops, uint32_t addr, size_t size) {
    uint8_t buf[32];

    if ((ops == NULL) || (ops->read == NULL) || (size > sizeof(buf))) {
        return false;
    }
    if (ops->read(addr, buf, size) != 0) {
        return false;
    }
    for (size_t i = 0U; i < size; i++) {
        if (buf[i] != 0xFFU) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 找到下一条可写记录的位置；扇区满则擦除后从头开始
 * @return 0 = 成功
 */
static int log_next_slot(const boot_flash_ops_t *ops, uint32_t sector, size_t rec_size,
                         uint32_t *offset) {
    for (uint32_t off = 0U; (off + rec_size) <= BOOT_META_SECTOR_SIZE; off += rec_size) {
        if (record_is_erased(ops, sector + off, rec_size)) {
            *offset = off;
            return 0;
        }
    }
    if ((ops->erase_sector == NULL) || (ops->erase_sector(sector) != 0)) {
        return -1;
    }
    *offset = 0U;
    return 0;
}

static int log_append(const boot_flash_ops_t *ops, uint32_t sector, const void *rec,
                      size_t rec_size) {
    uint32_t offset;
    uint8_t verify[32];

    if (log_next_slot(ops, sector, rec_size, &offset) != 0) {
        return -1;
    }
    if ((ops->program == NULL) || (ops->program(sector + offset, rec, rec_size) != 0)) {
        return -1;
    }
    if ((ops->read(sector + offset, verify, rec_size) != 0)
        || (memcmp(verify, rec, rec_size) != 0)) {
        return -1;
    }
    return 0;
}

/* ============================================================================
 * 镜像日志
 * ============================================================================ */
static bool image_record_valid(const boot_image_record_t *rec) {
    if ((rec->magic != BOOT_IMAGE_REC_MAGIC)
        || ((rec->state != BOOT_IMAGE_STATE_FLASHING) && (rec->state != BOOT_IMAGE_STATE_READY))) {
        return false;
    }
    return boot_crc32(rec, sizeof(*rec) - sizeof(rec->record_crc32)) == rec->record_crc32;
}

int boot_image_record_load(const boot_flash_ops_t *ops, uint32_t sector_addr,
                           boot_image_record_t *out) {
    boot_image_record_t rec;
    boot_image_record_t latest;
    bool have_latest = false;

    for (uint32_t off = 0U; (off + sizeof(rec)) <= BOOT_META_SECTOR_SIZE; off += sizeof(rec)) {
        if (record_is_erased(ops, sector_addr + off, sizeof(rec))) {
            break;
        }
        if (ops->read(sector_addr + off, &rec, sizeof(rec)) != 0) {
            break;
        }
        if (!image_record_valid(&rec)) {
            continue; /* 半写/损坏记录：跳过，继续找后续有效记录 */
        }
        latest = rec;
        have_latest = true;
    }

    if (!have_latest) {
        return -1;
    }
    if (out != NULL) {
        *out = latest;
    }
    return 0;
}

static int image_record_append(const boot_flash_ops_t *ops, uint32_t sector_addr,
                               const boot_image_record_t *rec) {
    boot_image_record_t copy = *rec;

    copy.record_crc32 = boot_crc32(&copy, sizeof(copy) - sizeof(copy.record_crc32));
    return log_append(ops, sector_addr, &copy, sizeof(copy));
}

int boot_image_record_begin(const boot_flash_ops_t *ops, uint32_t sector_addr,
                            uint32_t image_size, uint32_t image_version) {
    boot_image_record_t rec = {0};

    rec.magic = BOOT_IMAGE_REC_MAGIC;
    rec.state = BOOT_IMAGE_STATE_FLASHING;
    rec.image_size = image_size;
    rec.image_crc32 = 0U;
    rec.image_version = image_version;
    return image_record_append(ops, sector_addr, &rec);
}

int boot_image_record_ready(const boot_flash_ops_t *ops, uint32_t sector_addr,
                            uint32_t image_size, uint32_t image_crc32, uint32_t image_version) {
    boot_image_record_t rec = {0};

    rec.magic = BOOT_IMAGE_REC_MAGIC;
    rec.state = BOOT_IMAGE_STATE_READY;
    rec.image_size = image_size;
    rec.image_crc32 = image_crc32;
    rec.image_version = image_version;
    return image_record_append(ops, sector_addr, &rec);
}

/* ============================================================================
 * 启动控制日志
 * ============================================================================ */
static bool control_record_valid(const boot_control_record_t *rec) {
    if ((rec->magic != BOOT_CONTROL_MAGIC) || (rec->active > 1U)) {
        return false;
    }
    if ((rec->pending != BOOT_SLOT_NONE) && (rec->pending > 1U)) {
        return false;
    }
    return boot_crc32(rec, sizeof(*rec) - sizeof(rec->crc32)) == rec->crc32;
}

/** 16 位序号回绕比较：a 比 b 新则返回 true */
static bool seq_newer(uint16_t a, uint16_t b) {
    return (int16_t) (a - b) > 0;
}

int boot_control_load(const boot_flash_ops_t *ops, boot_control_record_t *out) {
    boot_control_record_t rec;
    boot_control_record_t latest;
    bool have_latest = false;

    for (uint32_t off = 0U; (off + sizeof(rec)) <= BOOT_META_SECTOR_SIZE; off += sizeof(rec)) {
        if (record_is_erased(ops, BOOT_CONTROL_ADDR + off, sizeof(rec))) {
            break;
        }
        if (ops->read(BOOT_CONTROL_ADDR + off, &rec, sizeof(rec)) != 0) {
            break;
        }
        if (!control_record_valid(&rec)) {
            continue;
        }
        if (!have_latest || seq_newer(rec.seq, latest.seq)) {
            latest = rec;
            have_latest = true;
        }
    }

    if (!have_latest) {
        return -1;
    }
    if (out != NULL) {
        *out = latest;
    }
    return 0;
}

int boot_control_save(const boot_flash_ops_t *ops, const boot_control_record_t *rec) {
    boot_control_record_t latest;
    boot_control_record_t copy = *rec;

    if (boot_control_load(ops, &latest) == 0) {
        copy.seq = (uint16_t) (latest.seq + 1U);
    } else {
        copy.seq = 0U;
    }
    copy.magic = BOOT_CONTROL_MAGIC;
    copy.crc32 = boot_crc32(&copy, sizeof(copy) - sizeof(copy.crc32));
    return log_append(ops, BOOT_CONTROL_ADDR, &copy, sizeof(copy));
}

void boot_control_defaults(boot_control_record_t *rec) {
    memset(rec, 0, sizeof(*rec));
    rec->magic = BOOT_CONTROL_MAGIC;
    rec->active = 0U;
    rec->pending = BOOT_SLOT_NONE;
    rec->attempts = 0U;
    rec->confirmed = 0U;
}

int boot_confirm_running_slot(const boot_flash_ops_t *ops, uint8_t slot_id) {
    boot_control_record_t rec;

    if (slot_id >= BOOT_SLOT_COUNT) {
        return -1;
    }
    if (boot_control_load(ops, &rec) != 0) {
        /* 首次启动（J-Link 直烧，无控制记录）：补一条完整记录 */
        boot_control_defaults(&rec);
        rec.active = slot_id;
        rec.confirmed = 1U;
        return boot_control_save(ops, &rec);
    }

    if ((rec.active == slot_id) && (rec.pending == BOOT_SLOT_NONE) && (rec.confirmed != 0U)) {
        return 0; /* 已确认，不再写 flash */
    }

    rec.active = slot_id;
    rec.pending = BOOT_SLOT_NONE;
    rec.attempts = 0U;
    rec.confirmed = 1U;
    return boot_control_save(ops, &rec);
}
