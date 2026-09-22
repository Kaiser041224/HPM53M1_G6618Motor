/**
 * @file    test_algo_encoder_snapshot.c
 * @brief   algo_encoder_snapshot 主机自测：坏帧发布、连续失败、seq/age、复用
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

#include "algo_encoder_snapshot.h"

/**
 * @brief 构造默认快照对象（跳变上限 100 count、失败上限 3、陈旧 1000 cycle）
 */
static void ctor_default(algo_encoder_snapshot_t *s) {
    algo_encoder_snapshot_cfg_t cfg = {
        .jump_limit_counts = 100,
        .fail_limit = 3U,
        .stale_cycles = 1000U,
    };
    algo_encoder_snapshot_ctor(s, &cfg);
}

void test_algo_encoder_snapshot(void) {
    algo_encoder_snapshot_t s;
    algo_encoder_sample_t snap;
    algo_encoder_input_t in;

    /* 1) 空对象：读回不一致/无效 */
    ctor_default(&s);
    CHECK(algo_encoder_snapshot_read(&s, 100U, &snap, 1U) == false);
    CHECK(snap.valid == false);
    CHECK(snap.seq == 0U);

    /* 2) 接受样本：raw 发布、seq 推进、timestamp 记录 */
    in.ok = true;
    in.raw = 1000U;
    in.timestamp_cycles = 5000U;
    CHECK(algo_encoder_snapshot_push(&s, &in) == ALGO_ENC_PUSH_ACCEPTED);
    CHECK(algo_encoder_snapshot_read(&s, 5100U, &snap, 1U) == true);
    CHECK(snap.raw == 1000U);
    CHECK(snap.source_raw == 1000U);
    CHECK(snap.seq == 1U);
    CHECK(snap.timestamp_cycles == 5000U);
    CHECK(snap.age_cycles == 100U);
    CHECK(snap.jumped == false);
    CHECK(snap.consecutive_fail == 0U);

    /* 3) 坏帧（跳变）：不得污染发布值；seq 不推进；计数并标记 jumped */
    in.raw = 30000U; /* delta = +29000 > 100 */
    in.timestamp_cycles = 9000U;
    CHECK(algo_encoder_snapshot_push(&s, &in) == ALGO_ENC_PUSH_HELD);
    CHECK(algo_encoder_snapshot_read(&s, 9000U, &snap, 1U) == true);
    CHECK(snap.raw == 1000U);        /* 仍是上一有效值（缺陷修复点） */
    CHECK(snap.source_raw == 30000U); /* 设备实际读到的坏值仅作诊断 */
    CHECK(snap.seq == 1U);            /* 未接受 → 序号不推进 */
    CHECK(snap.jumped == true);
    CHECK(snap.valid == true);        /* 未达失败上限 → 保持 */
    CHECK(s.jump_count == 1U);

    /* 4) 连续跳变达失败上限 → invalid */
    in.raw = 40000U;
    CHECK(algo_encoder_snapshot_push(&s, &in) == ALGO_ENC_PUSH_HELD);
    in.raw = 50000U;
    CHECK(algo_encoder_snapshot_push(&s, &in) == ALGO_ENC_PUSH_REJECTED);
    CHECK(algo_encoder_snapshot_read(&s, 10000U, &snap, 1U) == false);
    CHECK(snap.valid == false);
    CHECK(snap.consecutive_fail >= 3U);

    /* 5) 恢复：接受样本后连续失败清零、valid 恢复 */
    in.ok = true;
    in.raw = 50500U; /* 相对 prev=1000 仍超限？ */
    /* 上一有效仍是 1000，delta=49500 超限 → 仍判跳变；改为临近值 */
    in.raw = 1005U;
    CHECK(algo_encoder_snapshot_push(&s, &in) == ALGO_ENC_PUSH_ACCEPTED);
    CHECK(algo_encoder_snapshot_read(&s, 11000U, &snap, 1U) == true);
    CHECK(snap.raw == 1005U);
    CHECK(snap.valid == true);
    CHECK(snap.consecutive_fail == 0U);
    CHECK(snap.seq == 2U);

    /* 6) 读失败：不改变发布值/序号，标记 read_failed */
    in.ok = false;
    in.raw = 0U;
    in.timestamp_cycles = 12000U;
    CHECK(algo_encoder_snapshot_push(&s, &in) == ALGO_ENC_PUSH_REJECTED);
    CHECK(algo_encoder_snapshot_read(&s, 12000U, &snap, 1U) == false);
    CHECK(snap.read_failed == true);
    CHECK(snap.raw == 1005U);
    CHECK(snap.seq == 2U);

    /* 7) is_new：同 seq 复用 / 新 seq 更新 */
    CHECK(algo_encoder_snapshot_is_new(&snap, 2U) == false);
    CHECK(algo_encoder_snapshot_is_new(&snap, 1U) == true);

    /* 8) 陈旧判定 */
    in.ok = true;
    in.raw = 1010U;
    in.timestamp_cycles = 20000U;
    CHECK(algo_encoder_snapshot_push(&s, &in) == ALGO_ENC_PUSH_ACCEPTED);
    CHECK(algo_encoder_snapshot_read(&s, 21000U, &snap, 1U) == true);
    CHECK(snap.age_cycles == 1000U);
    CHECK(algo_encoder_snapshot_is_fresh(&snap, 1000U) == true);
    CHECK(algo_encoder_snapshot_is_fresh(&snap, 999U) == false);
    CHECK(algo_encoder_snapshot_read(&s, 22000U, &snap, 1U) == true);
    CHECK(snap.age_cycles == 2000U);
    CHECK(algo_encoder_snapshot_is_fresh(&snap, 1000U) == false);

    /* 9) 小步进接受（1010 -> 1015） */
    in.ok = true;
    in.raw = 1015U;
    in.timestamp_cycles = 30000U;
    CHECK(algo_encoder_snapshot_push(&s, &in) == ALGO_ENC_PUSH_ACCEPTED);
    CHECK(algo_encoder_snapshot_read(&s, 30000U, &snap, 1U) == true);
    CHECK(snap.raw == 1015U);
    CHECK(snap.seq == 4U);

    /* 10) wrap-safe 跳变：65500 -> 20（delta=+56，在 ±100 内 → 接受） */
    {
        algo_encoder_snapshot_t s2;

        ctor_default(&s2);
        in.ok = true;
        in.raw = 65500U;
        in.timestamp_cycles = 1000U;
        CHECK(algo_encoder_snapshot_push(&s2, &in) == ALGO_ENC_PUSH_ACCEPTED);
        in.raw = 20U;
        in.timestamp_cycles = 2000U;
        CHECK(algo_encoder_snapshot_push(&s2, &in) == ALGO_ENC_PUSH_ACCEPTED);
        CHECK(algo_encoder_snapshot_read(&s2, 2000U, &snap, 1U) == true);
        CHECK(snap.raw == 20U);
        CHECK(snap.seq == 2U);
    }

    /* 11) 有界读：max_retries=0 时无法取得一致快照 → 返回 false */
    CHECK(algo_encoder_snapshot_read(&s, 30000U, &snap, 0U) == false);
}
