/**
 * @file    mock_flash.c
 * @brief   宿主测试替身：app_param 记录存储（多槽，支持长度不符注入）
 * @author  Kaiser
 *
 * 仅用于宿主机自测，不参与目标固件。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_param.h"

#include <string.h>

#define MOCK_FLASH_SLOTS (4U)

typedef struct {
    uint32_t key;
    uint8_t data[APP_PARAM_DATA_MAX];
    size_t length;
    bool used;
} mock_flash_slot_t;

static mock_flash_slot_t s_slots[MOCK_FLASH_SLOTS];

void mock_flash_reset(void) { memset(s_slots, 0, sizeof(s_slots)); }

bool app_param_is_ready(void) { return true; }

int app_param_store(uint32_t key, const void* buf, size_t len) {
    if (buf == NULL || len > APP_PARAM_DATA_MAX)
        return -1;
    mock_flash_slot_t* slot = NULL;
    for (unsigned i = 0; i < MOCK_FLASH_SLOTS; ++i) {
        if (s_slots[i].used && s_slots[i].key == key) {
            slot = &s_slots[i];
            break;
        }
    }
    if (slot == NULL) {
        for (unsigned i = 0; i < MOCK_FLASH_SLOTS; ++i) {
            if (!s_slots[i].used) {
                slot = &s_slots[i];
                break;
            }
        }
    }
    if (slot == NULL)
        return -1;
    memcpy(slot->data, buf, len);
    slot->key = key;
    slot->length = len;
    slot->used = true;
    return 0;
}

int app_param_load(uint32_t key, void* buf, size_t len) {
    if (buf == NULL)
        return -1;
    for (unsigned i = 0; i < MOCK_FLASH_SLOTS; ++i) {
        if (s_slots[i].used && s_slots[i].key == key) {
            if (s_slots[i].length != len) /* 长度不符（旧 schema 记录） */
                return -1;
            memcpy(buf, s_slots[i].data, len);
            return 0;
        }
    }
    return -1;
}

int app_param_erase(uint32_t key) {
    for (unsigned i = 0; i < MOCK_FLASH_SLOTS; ++i) {
        if (s_slots[i].used && s_slots[i].key == key) {
            s_slots[i].used = false;
            s_slots[i].length = 0;
            return 0;
        }
    }
    return 0;
}

int app_param_load_flex(uint32_t key, void* buf, size_t cap, size_t* out_len) {
    if (buf == NULL || out_len == NULL)
        return -1;
    for (unsigned i = 0; i < MOCK_FLASH_SLOTS; ++i) {
        if (s_slots[i].used && s_slots[i].key == key) {
            if (s_slots[i].length > cap) /* 记录长于调用方缓冲 */
                return -1;
            memcpy(buf, s_slots[i].data, s_slots[i].length);
            *out_len = s_slots[i].length;
            return 0;
        }
    }
    return -1;
}
