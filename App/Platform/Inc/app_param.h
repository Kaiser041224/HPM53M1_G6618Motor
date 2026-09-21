/**
 * @file    app_param.h
 * @brief   片内 Flash 键值参数存储（通用，掉电保持）
 * @author  Kaiser
 *
 * 记录槽（128B；扇区 4KB → 32 槽）：
 *   magic(4) + key(4) + version(2) + length(2) + data[112] + crc32(4)
 *
 * 语义：
 *   - 按 key 读写：各模块自定义 key 与 data 布局，互不影响
 *     （key 集中登记于本文件顶部，避免冲突）
 *   - store = 整扇区读-改-写（1 次擦除）+ 回读校验
 *   - load  = 逐槽扫描 + magic/CRC32 校验，失败返回 -1
 *   - 存储位置：flash 参数扇区（倒数第 2 个扇区，链接脚本已预留）
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_PARAM_H
#define APP_PARAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 参数键登记（各模块在此登记，避免冲突）
 * ============================================================================ */
#define APP_PARAM_KEY_ENCODER (0x454E4344U) /* "ENCD"：编码器零点等 */

/* 单条记录最大数据长度（128 - 头 12 - CRC 4） */
#define APP_PARAM_DATA_MAX (112U)

/**
 * @brief 初始化参数存储：注册 flash 驱动、初始化 flash。
 * @return 0 = 可用；-1 = flash 不可用（读写将全部失败）
 */
int app_param_init(void);

/**
 * @brief 存储是否可用（flash 初始化成功且扇区规格匹配）。
 */
bool app_param_is_ready(void);

/**
 * @brief 读取参数：查找 key 对应的有效记录并拷贝。
 * @param key 参数键
 * @param buf 输出缓冲
 * @param len 期望长度（必须与记录长度一致）
 * @return 0 = 成功；-1 = 未找到 / 校验失败 / 长度不符 / 存储不可用
 */
int app_param_load(uint32_t key, void* buf, size_t len);

/**
 * @brief 保存参数：更新（或新增）key 对应记录（整扇区读-改-写 + 回读校验）。
 * @return 0 = 成功；-1 = 失败
 */
int app_param_store(uint32_t key, const void* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* APP_PARAM_H */
