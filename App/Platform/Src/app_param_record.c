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
    uint8_t buffer[APP_PARAM_DATA_MAX];
    app_param_record_header_t header;

    if (payload == NULL || payload_size == 0 || payload_size + sizeof(header) > APP_PARAM_DATA_MAX)
        return -1;
    if (!app_param_is_ready())
        return -1;
    if (app_param_load(key, buffer, sizeof(header) + payload_size) != 0)
        return -1;
    memcpy(&header, buffer, sizeof(header));
    /* 整条有效才叠加：代次或长度不符（旧schema/本次烧录清除）→ 忽略，payload 不改 */
    if (header.generation != generation || header.payload_size != payload_size)
        return -1;
    memcpy(payload, buffer + sizeof(header), payload_size);
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
    header.payload_size = (uint32_t)payload_size;
    memcpy(buffer, &header, sizeof(header));
    memcpy(buffer + sizeof(header), payload, payload_size);
    return app_param_store(key, buffer, sizeof(header) + payload_size);
}
