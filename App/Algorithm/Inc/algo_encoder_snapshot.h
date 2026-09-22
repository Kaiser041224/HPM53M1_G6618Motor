/**
 * @file    algo_encoder_snapshot.h
 * @brief   编码器样本一致快照（seqlock）+ 坏帧/陈旧策略 — 纯算法，零硬件依赖
 * @author  Kaiser
 *
 * 设计（详见 docs/superpowers/specs/2026-09-22-encoder-sampler-foc-realtime-design.md）：
 *   - 单一写者（采样中断）：push() 接受/拒绝样本，仅在接受时更新 raw/seq/timestamp
 *   - 多读者（更高中断优先级 ISR / 主循环 / 调试）：read() 有界重试，绝不自旋
 *   - 坏帧（跳变）不污染已发布 raw；连续失败/跳变达 fail_limit → valid=false
 *   - 消费方用 is_new() 判复用（12.5kHz 样本可被 25kHz 环复用两次），
 *     用 is_fresh() + age_cycles 判时效，dt 由 timestamp_cycles 差值得到
 *
 * 线程/中断假设：
 *   - 仅一个写者
 *   - ISR 读者优先级高于写者（写者无法打断它）→ 理论无撕裂；仍以有界读兜底
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ALGO_ENCODER_SNAPSHOT_H
#define ALGO_ENCODER_SNAPSHOT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 默认连续失败/跳变上限（达到即快照无效） */
#define ALGO_ENCODER_FAIL_LIMIT_DEFAULT (3U)

/**
 * @brief push() 结果
 */
typedef enum {
    ALGO_ENC_PUSH_ACCEPTED = 0, /**< 正常接受：raw/seq/timestamp 更新 */
    ALGO_ENC_PUSH_HELD,         /**< 跳变但未达失败上限：保持上一有效值 */
    ALGO_ENC_PUSH_REJECTED,     /**< 读失败或连续跳变达上限：快照无效 */
} algo_encoder_push_result_t;

/**
 * @brief 样本输入（采样器给出）
 */
typedef struct {
    uint16_t raw;              /**< 本次设备读到的原始值（ok=false 时忽略） */
    bool     ok;               /**< 设备读是否成功 */
    uint32_t timestamp_cycles; /**< 本次采样时刻 [cycle] */
} algo_encoder_input_t;

/**
 * @brief 一致快照内容（读者可见）
 */
typedef struct {
    uint16_t raw;              /**< 最近被接受的原始值（坏帧不写入） */
    uint16_t source_raw;       /**< 最近一次设备读到的原始值（含被拒，仅诊断） */
    uint32_t seq;              /**< 接受样本计数（仅接受时 +1） */
    uint32_t timestamp_cycles; /**< 最近接受样本时刻 [cycle] */
    uint32_t age_cycles;       /**< 读取时计算的年龄 [cycle] */
    uint16_t consecutive_fail; /**< 连续失败/跳变计数 */
    bool     valid;            /**< 快照有效（读成功且未达连续失败上限） */
    bool     jumped;           /**< 最近样本因跳变被拒 */
    bool     read_failed;      /**< 最近设备读失败 */
} algo_encoder_sample_t;

/**
 * @brief 构造配置
 */
typedef struct {
    int32_t  jump_limit_counts; /**< 单步跳变上限 [count]（≤0 = 关闭跳变检测） */
    uint16_t fail_limit;        /**< 连续失败/跳变上限（0 = 默认 3） */
    uint32_t stale_cycles;      /**< 时效阈值 [cycle]（0 = 不判陈旧） */
} algo_encoder_snapshot_cfg_t;

/**
 * @brief 快照对象（单一写者）
 */
typedef struct {
    volatile uint32_t generation;   /**< seqlock 代际（奇 = 写入中） */
    algo_encoder_sample_t sample;   /**< 已发布 payload */

    int32_t  jump_limit;            /**< 配置：跳变上限 */
    uint16_t fail_limit;            /**< 配置：失败上限 */
    uint32_t stale_cycles;          /**< 配置：时效阈值 */

    uint16_t prev_raw;              /**< 最近接受 raw（写者私有） */
    bool     prev_valid;            /**< 已建立上一有效值 */

    volatile uint32_t jump_count;   /**< 累计跳变次数 */
    volatile uint32_t error_count;  /**< 累计读失败次数 */
} algo_encoder_snapshot_t;

/**
 * @brief 构造快照对象（清零状态并应用配置）
 * @param self 对象
 * @param cfg 配置（可为 NULL，全默认）
 */
void algo_encoder_snapshot_ctor(algo_encoder_snapshot_t* self,
                                const algo_encoder_snapshot_cfg_t* cfg);

/**
 * @brief 推入一个样本（写者，中断上下文）
 * @param self 对象
 * @param in 样本输入
 * @return 见 algo_encoder_push_result_t
 */
int algo_encoder_snapshot_push(algo_encoder_snapshot_t* self, const algo_encoder_input_t* in);

/**
 * @brief 有界读取一致快照（读者）
 * @param self 对象
 * @param now_cycles 当前时刻 [cycle]（用于计算 age）
 * @param out 输出快照
 * @param max_retries 最大重试次数（ISR 建议 1；主循环可 8；0 = 不读）
 * @return true = 取到一致且有效的快照；false = 不一致/重试耗尽/无效
 * @note 读取失败时 out 仍被填充（可查看 valid/read_failed 诊断）
 */
bool algo_encoder_snapshot_read(algo_encoder_snapshot_t* self, uint32_t now_cycles,
                                algo_encoder_sample_t* out, uint8_t max_retries);

/**
 * @brief 本快照是否为相对 last_seq 的新样本（12.5kHz→25kHz 复用检测）
 * @param snap 快照
 * @param last_seq 消费方上次处理的序号
 * @return true = 新样本
 */
bool algo_encoder_snapshot_is_new(const algo_encoder_sample_t* snap, uint32_t last_seq);

/**
 * @brief 快照时效判定
 * @param snap 快照
 * @param max_age_cycles 最大允许年龄 [cycle]（0 = 不判陈旧）
 * @return true = 有效且未超龄
 */
bool algo_encoder_snapshot_is_fresh(const algo_encoder_sample_t* snap, uint32_t max_age_cycles);

#ifdef __cplusplus
}
#endif

#endif /* ALGO_ENCODER_SNAPSHOT_H */
