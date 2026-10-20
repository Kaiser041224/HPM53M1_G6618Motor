/**
 * @file    app_param_record.c
 * @brief   带代次参数记录读写实现（头+负载，整条有效性判定）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_param_record.h"

#include "app_param.h"

#include <string.h>

int app_param_record_load(uint32_t key, void* payload, size_t payload_size, uint32_t generation) {
    return app_param_record_load_prefix(
        key, payload, payload_size, payload_size, generation);
}

int app_param_record_load_prefix(
    uint32_t key, void* payload, size_t payload_cap, size_t min_payload_size,
    uint32_t generation) {
    uint8_t buffer[APP_PARAM_DATA_MAX];
    app_param_record_header_t header;
    size_t stored = 0U;

    if (payload == NULL || payload_cap == 0U || min_payload_size == 0U
        || min_payload_size > payload_cap)
        return -1;
    if (!app_param_is_ready())
        return -1;
    if (app_param_load_flex(key, buffer, sizeof(buffer), &stored) != 0)
        return -1;
    if (stored < sizeof(header))
        return -1;
    memcpy(&header, buffer, sizeof(header));
    /* 整条有效才叠加：代次不符（本次烧录清除）→ 忽略，payload 不改 */
    if (header.generation != generation)
        return -1;
    size_t payload_len = stored - sizeof(header);
    if (payload_len < min_payload_size || payload_len > payload_cap)
        return -1;
    memcpy(payload, buffer + sizeof(header), payload_len);
    return 0;
}

int app_param_record_store(
    uint32_t key, const void* payload, size_t payload_size, uint32_t generation) {
    uint8_t buffer[APP_PARAM_DATA_MAX];
    app_param_record_header_t header;

    if (payload == NULL || payload_size == 0 || payload_size + sizeof(header) > APP_PARAM_DATA_MAX)
        return -1;
    if (!app_param_is_ready())
        return -1;
    header.generation = generation;
    memcpy(buffer, &header, sizeof(header));
    memcpy(buffer + sizeof(header), payload, payload_size);
    return app_param_store(key, buffer, sizeof(header) + payload_size);
}
