/**
 * @file    algo_encoder_snapshot.c
 * @brief   编码器样本一致快照实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "algo_encoder_snapshot.h"

#include <stdatomic.h>
#include <stddef.h>

#define ALGO_ENC_WRAP_HALF (32768)

/**
 * @brief 发布 payload（写者）：代际变奇 → 写 → 变偶
 * @param self 对象
 * @param sample 待发布样本
 */
static void algo_encoder_publish(algo_encoder_snapshot_t* self,
                                 const algo_encoder_sample_t* sample) {
    self->generation++;
    atomic_signal_fence(memory_order_seq_cst);
    self->sample = *sample;
    atomic_signal_fence(memory_order_seq_cst);
    self->generation++;
}

void algo_encoder_snapshot_ctor(algo_encoder_snapshot_t* self,
                                const algo_encoder_snapshot_cfg_t* cfg) {
    if (self == NULL) {
        return;
    }

    self->generation = 0U;
    self->sample = (algo_encoder_sample_t){0};
    self->prev_raw = 0U;
    self->prev_valid = false;
    self->jump_count = 0U;
    self->error_count = 0U;

    if (cfg != NULL) {
        self->jump_limit = cfg->jump_limit_counts;
        self->fail_limit = (cfg->fail_limit != 0U) ? cfg->fail_limit
                                                   : ALGO_ENCODER_FAIL_LIMIT_DEFAULT;
        self->stale_cycles = cfg->stale_cycles;
    } else {
        self->jump_limit = 0;
        self->fail_limit = ALGO_ENCODER_FAIL_LIMIT_DEFAULT;
        self->stale_cycles = 0U;
    }
}

int algo_encoder_snapshot_push(algo_encoder_snapshot_t* self, const algo_encoder_input_t* in) {
    algo_encoder_sample_t sample;
    bool jumped = false;

    if ((self == NULL) || (in == NULL)) {
        return ALGO_ENC_PUSH_REJECTED;
    }

    sample = self->sample; /* 以当前发布值为基础（保持语义） */

    if (!in->ok) {
        self->error_count++;
        if (sample.consecutive_fail < UINT16_MAX) {
            sample.consecutive_fail++;
        }
        sample.read_failed = true;
        sample.jumped = false;
        sample.valid = false; /* 无新数据：对消费者立即无效，但保留 raw/seq 供诊断 */
        sample.source_raw = sample.raw;
        algo_encoder_publish(self, &sample);
        return ALGO_ENC_PUSH_REJECTED;
    }

    /* 跳变检测（wrap-safe，仅在有上一有效值时） */
    if (self->prev_valid && (self->jump_limit > 0)) {
        int32_t delta = (int32_t)in->raw - (int32_t)self->prev_raw;

        if (delta > ALGO_ENC_WRAP_HALF) {
            delta -= 65536;
        } else if (delta < -ALGO_ENC_WRAP_HALF) {
            delta += 65536;
        }
        if ((delta > self->jump_limit) || (delta < -self->jump_limit)) {
            jumped = true;
        }
    }

    if (jumped) {
        self->jump_count++;
        if (sample.consecutive_fail < UINT16_MAX) {
            sample.consecutive_fail++;
        }
        sample.jumped = true;
        sample.read_failed = false;
        sample.source_raw = in->raw; /* 设备实际读到的坏值：仅诊断，不发布为 raw */
        if (sample.consecutive_fail >= self->fail_limit) {
            sample.valid = false;
        }
        algo_encoder_publish(self, &sample);
        return (sample.valid) ? ALGO_ENC_PUSH_HELD : ALGO_ENC_PUSH_REJECTED;
    }

    /* 接受：唯一更新 raw/seq/timestamp 的路径 */
    self->prev_raw = in->raw;
    self->prev_valid = true;
    sample.raw = in->raw;
    sample.source_raw = in->raw;
    sample.seq++;
    sample.timestamp_cycles = in->timestamp_cycles;
    sample.consecutive_fail = 0U;
    sample.valid = true;
    sample.jumped = false;
    sample.read_failed = false;
    algo_encoder_publish(self, &sample);
    return ALGO_ENC_PUSH_ACCEPTED;
}

bool algo_encoder_snapshot_read(algo_encoder_snapshot_t* self, uint32_t now_cycles,
                                algo_encoder_sample_t* out, uint8_t max_retries) {
    if ((self == NULL) || (out == NULL)) {
        return false;
    }

    for (uint8_t attempt = 0U; attempt < max_retries; attempt++) {
        uint32_t gen0 = self->generation;
        algo_encoder_sample_t tmp;

        if ((gen0 & 1U) != 0U) {
            continue; /* 写入中 */
        }
        atomic_signal_fence(memory_order_seq_cst);
        tmp = self->sample;
        atomic_signal_fence(memory_order_seq_cst);
        if (gen0 != self->generation) {
            continue; /* 读期间被写入：重试 */
        }

        tmp.age_cycles = now_cycles - tmp.timestamp_cycles;
        *out = tmp;
        return tmp.valid;
    }

    /* 重试耗尽：返回当前发布值（可能不一致/无效），不阻塞 */
    *out = self->sample;
    out->age_cycles = now_cycles - out->timestamp_cycles;
    return false;
}

bool algo_encoder_snapshot_is_new(const algo_encoder_sample_t* snap, uint32_t last_seq) {
    if (snap == NULL) {
        return false;
    }
    return (snap->seq != last_seq);
}

bool algo_encoder_snapshot_is_fresh(const algo_encoder_sample_t* snap, uint32_t max_age_cycles) {
    if ((snap == NULL) || !snap->valid) {
        return false;
    }
    if (max_age_cycles == 0U) {
        return true;
    }
    return (snap->age_cycles <= max_age_cycles);
}
