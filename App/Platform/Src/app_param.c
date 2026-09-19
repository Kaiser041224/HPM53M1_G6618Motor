/*
 * App Param - 片内 Flash 键值参数存储（通用，掉电保持）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_param.h"

#include "intf_flash.h"

#include <string.h>

/* 驱动注册（App 层不含 hpm_* 头文件，沿用既有 extern 约定） */
extern void hpm_flash_driver_register(void);

#define APP_PARAM_MAGIC        (0x504D5048U) /* "HPMP" */
#define APP_PARAM_VERSION      (1U)
#define APP_PARAM_SLOT_SIZE    (128U)
#define APP_PARAM_HDR_SIZE     (12U) /* magic + key + version + length */
#define APP_PARAM_SLOT_MAX     (32U) /* 4KB 扇区 / 128B */

typedef struct {
    uint32_t magic;
    uint32_t key;
    uint16_t version;
    uint16_t length; /* data 有效字节数 */
    uint8_t  data[APP_PARAM_DATA_MAX];
    uint32_t crc32; /* 覆盖 magic..data[length-1] */
} app_param_slot_t;

static const intf_flash_t *s_flash;
static uint32_t s_sector_addr;
static uint32_t s_slot_count;
static bool s_ready;

/* 保存时的整扇区缓冲（仅 store 使用） */
static app_param_slot_t s_slots[APP_PARAM_SLOT_MAX];

/* CRC-32/ISO-HDLC（反射多项式 0xEDB88320） */
static uint32_t param_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0U; i < len; i++) {
        crc ^= data[i];
        for (uint32_t bit = 0U; bit < 8U; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1U)));
        }
    }
    return ~crc;
}

static bool param_slot_valid(const app_param_slot_t *slot)
{
    if (slot->magic != APP_PARAM_MAGIC) {
        return false;
    }
    if (slot->length > APP_PARAM_DATA_MAX) {
        return false;
    }
    return param_crc32((const uint8_t *) slot, APP_PARAM_HDR_SIZE + slot->length) == slot->crc32;
}

int app_param_init(void)
{
    uint32_t sector;

    hpm_flash_driver_register();

    s_flash = intf_flash_get();
    if ((s_flash == NULL) || (s_flash->init() != 0)) {
        return -1;
    }

    sector = s_flash->get_sector_size();
    if ((sector == 0U) || (sector % APP_PARAM_SLOT_SIZE) != 0U ||
        ((sector / APP_PARAM_SLOT_SIZE) > APP_PARAM_SLOT_MAX)) {
        return -1; /* 扇区规格与槽布局不匹配 */
    }

    /* 参数扇区 = flash 末尾倒数第 2 个扇区 */
    s_sector_addr = s_flash->get_base_addr() + s_flash->get_size() - (2U * sector);
    s_slot_count = sector / APP_PARAM_SLOT_SIZE;
    s_ready = true;

    return 0;
}

bool app_param_is_ready(void)
{
    return s_ready;
}

int app_param_load(uint32_t key, void *buf, size_t len)
{
    app_param_slot_t slot;

    if (!s_ready || (buf == NULL) || (len == 0U) || (len > APP_PARAM_DATA_MAX)) {
        return -1;
    }

    for (uint32_t i = 0U; i < s_slot_count; i++) {
        memset(&slot, 0, sizeof(slot));
        if (s_flash->read(s_sector_addr + (i * APP_PARAM_SLOT_SIZE), &slot, sizeof(slot)) != 0) {
            return -1;
        }
        if (param_slot_valid(&slot) && (slot.key == key)) {
            if (slot.length != len) {
                return -1;
            }
            memcpy(buf, slot.data, len);
            return 0;
        }
    }

    return -1; /* 未找到 */
}

int app_param_store(uint32_t key, const void *buf, size_t len)
{
    app_param_slot_t verify;
    uint32_t target = s_slot_count;

    if (!s_ready || (buf == NULL) || (len == 0U) || (len > APP_PARAM_DATA_MAX)) {
        return -1;
    }

    /* 1) 读整扇区 */
    if (s_flash->read(s_sector_addr, s_slots, s_slot_count * APP_PARAM_SLOT_SIZE) != 0) {
        return -1;
    }

    /* 2) 定位槽位：已有 key 的槽，否则第一个空槽 */
    for (uint32_t i = 0U; i < s_slot_count; i++) {
        if (param_slot_valid(&s_slots[i]) && (s_slots[i].key == key)) {
            target = i;
            break;
        }
    }
    if (target == s_slot_count) {
        for (uint32_t i = 0U; i < s_slot_count; i++) {
            if (!param_slot_valid(&s_slots[i])) {
                target = i;
                break;
            }
        }
    }
    if (target == s_slot_count) {
        return -1; /* 槽位已满 */
    }

    /* 3) 构造记录 */
    memset(&s_slots[target], 0, APP_PARAM_SLOT_SIZE);
    s_slots[target].magic = APP_PARAM_MAGIC;
    s_slots[target].key = key;
    s_slots[target].version = APP_PARAM_VERSION;
    s_slots[target].length = (uint16_t) len;
    memcpy(s_slots[target].data, buf, len);
    s_slots[target].crc32 =
        param_crc32((const uint8_t *) &s_slots[target], APP_PARAM_HDR_SIZE + len);

    /* 4) 擦除 + 整扇区写回 */
    if (s_flash->erase_sector(s_sector_addr) != 0) {
        return -1;
    }
    if (s_flash->program(s_sector_addr, s_slots, s_slot_count * APP_PARAM_SLOT_SIZE) != 0) {
        return -1;
    }

    /* 5) 回读校验（本槽） */
    memset(&verify, 0, sizeof(verify));
    if (s_flash->read(s_sector_addr + (target * APP_PARAM_SLOT_SIZE), &verify, sizeof(verify)) != 0) {
        return -1;
    }
    if (memcmp(&verify, &s_slots[target], sizeof(verify)) != 0) {
        return -1;
    }

    return 0;
}
