/**
 * @file mock_param.c
 * @brief 参数存储替身
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_param.h"
#include <string.h>
static unsigned char s_record[APP_PARAM_DATA_MAX];
static size_t s_length;
static uint32_t s_key;
int g_mock_param_stores;
void mock_param_store_reset(void) {
    s_length = 0;
    g_mock_param_stores = 0;
}
bool app_param_is_ready(void) { return true; }
int app_param_store(uint32_t key, const void* buf, size_t len) {
    if (len > sizeof(s_record))
        return -1;
    memcpy(s_record, buf, len);
    s_key = key;
    s_length = len;
    ++g_mock_param_stores;
    return 0;
}
int app_param_load(uint32_t key, void* buf, size_t len) {
    if (key != s_key || len != s_length)
        return -1;
    memcpy(buf, s_record, len);
    return 0;
}
int app_param_erase(uint32_t key) {
    if (key != s_key)
        return 0;
    s_length = 0;
    s_key = 0;
    return 0;
}
