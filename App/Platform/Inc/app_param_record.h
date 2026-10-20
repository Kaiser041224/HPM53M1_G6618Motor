/**
 * @file    app_param_record.h
 * @brief   带代次参数记录读写（flash 记忆值覆盖出厂值的通用载体）
 * @author  Kaiser
 *
 * 记录布局：头 {generation} + payload（域结构体原始字节）；长度不符整条拒绝。
 * 代次语义：与软件参数 params_generation 一致才叠加；不一致（bump 后烧录）或
 * 长度不符（旧 schema）时整条忽略，调用方保持出厂值 —— 即"清除并替换"。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_PARAM_RECORD_H
#define APP_PARAM_RECORD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 记录头（代次；负载长度由 app_param 记录长度校验兜底，不重复存储） */
typedef struct {
    uint32_t generation; /**< 写入时的 params_generation */
} app_param_record_header_t;

/**
 * @brief 读取记录并叠加到 payload（仅整条有效时覆盖）
 * @param key 记录键（app_param 4字符键）
 * @param payload 负载缓冲（域结构体）
 * @param payload_size 负载字节数（须与记录一致）
 * @param generation 当前固件代次（software.params_generation）
 * @return 0 = flash 记录有效并已叠加；-1 = 无记录/代次或长度不符/存储不可用（payload 不改）
 */
int app_param_record_load(uint32_t key, void* payload, size_t payload_size, uint32_t generation);

/**
 * @brief 读取记录并叠加到 payload 前缀（允许旧版短记录，供结构体追加字段迁移）
 * @param key 记录键
 * @param payload 负载缓冲（先由调用方填出厂默认）
 * @param payload_cap 负载缓冲容量（新结构体大小）
 * @param min_payload_size 可接受的最小负载字节数（旧结构体大小）
 * @param generation 当前固件代次
 * @return 0 = 有效记录已拷贝（长度为 min..cap）；-1 = 无记录/代次或长度不符
 * @note 只拷贝记录中的实际字节；新增字段保持调用方预填的出厂默认。
 */
int app_param_record_load_prefix(
    uint32_t key, void* payload, size_t payload_cap, size_t min_payload_size,
    uint32_t generation);

/**
 * @brief 写入记录（头 + 负载，整记录覆盖写）
 * @return 0 = 成功；-1 = 非法参数/超出记录槽/写入失败
 */
int app_param_record_store(
    uint32_t key, const void* payload, size_t payload_size, uint32_t generation);

#ifdef __cplusplus
}
#endif

#endif /* APP_PARAM_RECORD_H */
