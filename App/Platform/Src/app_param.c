/**
 * @file    app_param.c
 * @brief   片内 Flash 键值参数存储（通用，掉电保持）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_param.h"

#include "intf_flash.h"

#include <string.h>

/* 驱动注册（App 层不含 hpm_* 头文件，沿用既有 extern 约定） */
extern void hpm_flash_driver_register(void);

#define APP_PARAM_MAGIC     (0x504D5048U) /* "HPMP" */
#define APP_PARAM_VERSION   (1U)
#define APP_PARAM_SLOT_SIZE (256U)
#define APP_PARAM_HDR_SIZE  (12U)         /* magic + key + version + length */
#define APP_PARAM_SLOT_MAX  (16U)         /* 4KB 扇区 / 256B */

/**
 * @brief 单个参数槽记录（头 + 数据 + CRC）
 */
typedef struct {
    uint32_t magic;                   /**< 魔数 "HPMP" */
    uint32_t key;                     /**< 参数键 */
    uint16_t version;                 /**< 记录版本 */
    uint16_t length;                  /**< data 有效字节数 */
    uint8_t data[APP_PARAM_DATA_MAX]; /**< 参数数据 */
    uint32_t crc32;                   /**< 覆盖 magic..data[length-1] */
} app_param_slot_t;

static const intf_flash_t* s_flash;
static uint32_t s_sector_addr;
static uint32_t s_slot_count;
static bool s_ready;

/* Ozone 停机定位：flash 保存卡住时读这两个 noncacheable 变量。
 * stage: 0=idle, 1=read, 2=find, 3=erase, 4=program, 5=verify, 6=done */
volatile uint32_t g_param_store_stage __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_param_store_slot __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_param_store_error __attribute__((section(".noncacheable.bss")));

/* 保存时的整扇区缓冲（仅 store 使用） */
static app_param_slot_t s_slots[APP_PARAM_SLOT_MAX];
static app_param_slot_t s_load_slot;   /**< load 读暂存（避免 256B 栈帧；load/store 分缓冲，
                                        *  运行期 terminal(store)×diag/init(load) 交叉可重入） */
static app_param_slot_t s_verify_slot; /**< store 回读校验暂存（同上） */

/**
 * @brief CRC-32/ISO-HDLC 校验（反射多项式 0xEDB88320）
 * @param data 数据指针
 * @param len 数据长度
 * @return CRC32 值
 */
static uint32_t param_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0U; i < len; i++) {
        crc ^= data[i];
        for (uint32_t bit = 0U; bit < 8U; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1U)));
        }
    }
    return ~crc;
}

/**
 * @brief 校验参数槽：魔数、长度与 CRC
 * @param slot 待校验槽
 * @return true = 有效
 */
static bool param_slot_valid(const app_param_slot_t* slot) {
    if (slot->magic != APP_PARAM_MAGIC) {
        return false;
    }
    if (slot->length > APP_PARAM_DATA_MAX) {
        return false;
    }
    return param_crc32((const uint8_t*)slot, APP_PARAM_HDR_SIZE + slot->length) == slot->crc32;
}

int app_param_init(void) {
    uint32_t sector;

    if (s_ready) {
        return 0;
    }
    hpm_flash_driver_register();
    g_param_store_stage = 0U;
    g_param_store_slot = 0xFFFFFFFFU;
    g_param_store_error = 0U;

    s_flash = intf_flash_get();
    if ((s_flash == NULL) || (s_flash->init() != 0)) {
        return -1;
    }

    sector = s_flash->get_sector_size();
    if ((sector == 0U) || (sector % APP_PARAM_SLOT_SIZE) != 0U
        || ((sector / APP_PARAM_SLOT_SIZE) > APP_PARAM_SLOT_MAX)) {
        return -1; /* 扇区规格与槽布局不匹配 */
    }

    /* 参数扇区 = flash 末尾倒数第 2 个扇区 */
    s_sector_addr = s_flash->get_base_addr() + s_flash->get_size() - (2U * sector);
    s_slot_count = sector / APP_PARAM_SLOT_SIZE;
    s_ready = true;

    return 0;
}

bool app_param_is_ready(void) { return s_ready; }

int app_param_load(uint32_t key, void* buf, size_t len) {
    app_param_slot_t* slot = &s_load_slot;

    if (!s_ready || (buf == NULL) || (len == 0U) || (len > APP_PARAM_DATA_MAX)) {
        return -1;
    }

    for (uint32_t i = 0U; i < s_slot_count; i++) {
        memset(slot, 0, sizeof(*slot));
        if (s_flash->read(s_sector_addr + (i * APP_PARAM_SLOT_SIZE), slot, sizeof(*slot)) != 0) {
            return -1;
        }
        if (param_slot_valid(slot) && (slot->key == key)) {
            if (slot->length != len) {
                return -1;
            }
            memcpy(buf, slot->data, len);
            return 0;
        }
    }

    return -1; /* 未找到 */
}

int app_param_load_flex(uint32_t key, void* buf, size_t cap, size_t* out_len) {
    app_param_slot_t* slot = &s_load_slot;

    if (!s_ready || (buf == NULL) || (cap == 0U) || (cap > APP_PARAM_DATA_MAX)
        || (out_len == NULL)) {
        return -1;
    }

    for (uint32_t i = 0U; i < s_slot_count; i++) {
        memset(slot, 0, sizeof(*slot));
        if (s_flash->read(s_sector_addr + (i * APP_PARAM_SLOT_SIZE), slot, sizeof(*slot)) != 0) {
            return -1;
        }
        if (param_slot_valid(slot) && (slot->key == key)) {
            if (slot->length > cap) {
                return -1;
            }
            memcpy(buf, slot->data, slot->length);
            *out_len = slot->length;
            return 0;
        }
    }

    return -1; /* 未找到 */
}

int app_param_store(uint32_t key, const void* buf, size_t len) {
    app_param_slot_t* verify = &s_verify_slot;
    uint32_t target = s_slot_count;

    g_param_store_stage = 0U;
    g_param_store_slot = 0xFFFFFFFFU;
    g_param_store_error = 0U;
    if (!s_ready || (buf == NULL) || (len == 0U) || (len > APP_PARAM_DATA_MAX)) {
        g_param_store_error = 1U;
        return -1;
    }

    /* 1) 读整扇区 */
    g_param_store_stage = 1U;
    if (s_flash->read(s_sector_addr, s_slots, s_slot_count * APP_PARAM_SLOT_SIZE) != 0) {
        g_param_store_error = 2U;
        return -1;
    }
    g_param_store_stage = 2U;

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
    s_slots[target].length = (uint16_t)len;
    memcpy(s_slots[target].data, buf, len);
    s_slots[target].crc32 = param_crc32((const uint8_t*)&s_slots[target], APP_PARAM_HDR_SIZE + len);

    /* 4) 擦除 + 按有效槽写回（不写 0xFF 空槽）。
     *    4KB 整扇区一次 program 会拉长中断关闭窗口；这里按 256B 槽提交，
     *    既减少 flash 占用时间，也避免把大量空槽当成数据写入。 */
    g_param_store_stage = 3U;
    if (s_flash->erase_sector(s_sector_addr) != 0) {
        g_param_store_error = 3U;
        return -1;
    }
    g_param_store_stage = 4U;
    for (uint32_t i = 0U; i < s_slot_count; i++) {
        g_param_store_slot = i;
        if (!param_slot_valid(&s_slots[i])) {
            continue;
        }
        if (s_flash->program(s_sector_addr + (i * APP_PARAM_SLOT_SIZE), &s_slots[i],
                             APP_PARAM_SLOT_SIZE)
            != 0) {
            g_param_store_error = 4U;
            return -1;
        }
    }

    /* 5) 回读校验（本槽） */
    g_param_store_stage = 5U;
    memset(verify, 0, sizeof(*verify));
    if (s_flash->read(s_sector_addr + (target * APP_PARAM_SLOT_SIZE), verify, sizeof(*verify))
        != 0) {
        g_param_store_error = 5U;
        return -1;
    }
    if (memcmp(verify, &s_slots[target], sizeof(*verify)) != 0) {
        g_param_store_error = 6U;
        return -1;
    }

    g_param_store_stage = 6U;
    return 0;
}

int app_param_erase(uint32_t key) {
    uint32_t target = s_slot_count;
    app_param_slot_t* verify = &s_verify_slot;

    g_param_store_stage = 0U;
    g_param_store_slot = 0xFFFFFFFFU;
    g_param_store_error = 0U;
    if (!s_ready) {
        g_param_store_error = 1U;
        return -1;
    }

    /* 1) 读整扇区并定位 key；不存在直接成功返回，避免无意义擦写 */
    g_param_store_stage = 1U;
    if (s_flash->read(s_sector_addr, s_slots, s_slot_count * APP_PARAM_SLOT_SIZE) != 0) {
        g_param_store_error = 2U;
        return -1;
    }
    for (uint32_t i = 0U; i < s_slot_count; i++) {
        if (param_slot_valid(&s_slots[i]) && (s_slots[i].key == key)) {
            target = i;
            break;
        }
    }
    if (target == s_slot_count) {
        g_param_store_stage = 6U;
        return 0;
    }

    /* 2) 擦除整扇区，仅写回其余有效槽 */
    g_param_store_stage = 3U;
    if (s_flash->erase_sector(s_sector_addr) != 0) {
        g_param_store_error = 3U;
        return -1;
    }
    g_param_store_stage = 4U;
    for (uint32_t i = 0U; i < s_slot_count; i++) {
        if (i == target) {
            continue;
        }
        g_param_store_slot = i;
        if (!param_slot_valid(&s_slots[i])) {
            continue;
        }
        if (s_flash->program(s_sector_addr + (i * APP_PARAM_SLOT_SIZE), &s_slots[i],
                             APP_PARAM_SLOT_SIZE)
            != 0) {
            g_param_store_error = 4U;
            return -1;
        }
    }

    /* 3) 回读目标槽，确认已不再有效 */
    g_param_store_stage = 5U;
    memset(verify, 0, sizeof(*verify));
    if (s_flash->read(s_sector_addr + (target * APP_PARAM_SLOT_SIZE), verify, sizeof(*verify))
        != 0) {
        g_param_store_error = 5U;
        return -1;
    }
    if (param_slot_valid(verify)) {
        g_param_store_error = 6U;
        return -1;
    }

    g_param_store_stage = 6U;
    return 0;
}
