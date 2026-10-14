/**
 * @file    test_app_encoder_sync.c
 * @brief   app_encoder 同步采样（ADC 回调内 fetch，DMA 硬件触发帧）主机自测
 * @author  Kaiser
 *
 * 覆盖：
 *   1) 未 arm：sync_sample 拒绝（不触碰设备，out->valid=false）
 *   2) sync_start：TRGM 路由 PWM1 CH10REF → DMA_REQ0，设备 arm 一次，声明 SPI3 所有权
 *   3) 每拍 fetch → 快照发布（seq 推进、timestamp=调用方时刻、age=0）
 *   4) fetch 失败：读失败计数、快照不推进 seq，连续失败达上限后 invalid
 *   5) 运行期读 API 只读快照，不触发阻塞 SPI 读
 *   6) 坏帧（跳变）不污染已发布 raw
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

#include "app_encoder.h"
#include "intf_trgm.h"

extern void mock_reset(void);
extern int g_mock_enc_read_count[2];
extern uint16_t g_mock_enc_raw[2];
extern int g_mock_enc_sync_arm_count[2];
extern int g_mock_enc_sync_fetch_count[2];
extern int g_mock_enc_sync_fetch_fail[2];
extern uint8_t g_mock_enc_sync_trig[2];
extern int g_mock_trgm_connect_count;
extern intf_trgm_src_t g_mock_trgm_src;
extern intf_trgm_dst_t g_mock_trgm_dst;

void test_app_encoder_sync(void) {
    app_encoder_rotor_snapshot_t out;
    uint16_t raw = 0U;
    int reads_before;

    mock_reset();
    g_mock_enc_raw[APP_ENCODER_ROTOR] = 2000U;
    CHECK(app_encoder_init() == 0);
    CHECK(app_encoder_sync_active() == false);

    /* 1) 未 arm：拒绝且不触碰设备 */
    CHECK(app_encoder_sync_sample(100U, &out) != 0);
    CHECK(out.valid == false);
    CHECK(g_mock_enc_sync_fetch_count[APP_ENCODER_ROTOR] == 0);

    /* 2) 启动：TRGM 路由 + 设备 arm 一次 + 所有权声明 */
    CHECK(app_encoder_sync_start() == 0);
    CHECK(app_encoder_sync_active() == true);
    CHECK(app_encoder_sampler_active() == true);
    CHECK(g_mock_trgm_connect_count == 1);
    CHECK(g_mock_trgm_src == INTF_TRGM_SRC_PWM1_CH10REF);
    CHECK(g_mock_trgm_dst == INTF_TRGM_DST_DMA_REQ0);
    CHECK(g_mock_enc_sync_arm_count[APP_ENCODER_ROTOR] == 1);
    CHECK(g_mock_enc_sync_trig[APP_ENCODER_ROTOR] == 0U);      /* DMA_REQ0 */
    CHECK(g_mock_enc_sync_arm_count[APP_ENCODER_OUTPUT] == 0); /* 出轴不参与 */

    /* 3) 每拍 fetch → 发布 */
    CHECK(app_encoder_sync_sample(1000U, &out) == 0);
    CHECK(g_mock_enc_sync_fetch_count[APP_ENCODER_ROTOR] == 1);
    CHECK(out.valid == true);
    CHECK(out.raw == 2000U);
    CHECK(out.seq == 1U);
    CHECK(out.timestamp_cycles == 1000U);
    CHECK(out.age_cycles == 0U);
    CHECK_NEAR(out.rad, 2000.0f * 6.283185307179586f / 65536.0f, 1e-5f);

    g_mock_enc_raw[APP_ENCODER_ROTOR] = 2010U;
    CHECK(app_encoder_sync_sample(20200U, &out) == 0);
    CHECK(out.seq == 2U);
    CHECK(out.raw == 2010U);
    CHECK(out.timestamp_cycles == 20200U);

    /* 5) 运行期读 API：快照，不触发阻塞读 */
    reads_before = g_mock_enc_read_count[APP_ENCODER_ROTOR];
    CHECK(app_encoder_read_raw(APP_ENCODER_ROTOR, &raw) == 0);
    CHECK(raw == 2010U);
    CHECK(g_mock_enc_read_count[APP_ENCODER_ROTOR] == reads_before);

    /* 4) fetch 失败：计数 + seq 不推进；连续 3 次 → invalid */
    {
        uint32_t fail_before = g_encoder_read_fail_count;

        g_mock_enc_sync_fetch_fail[APP_ENCODER_ROTOR] = 1;
        CHECK(app_encoder_sync_sample(40200U, &out) != 0);
        CHECK(g_encoder_read_fail_count == fail_before + 1U);
        CHECK(out.seq == 2U);
        CHECK(out.raw == 2010U);
        CHECK(out.valid == false);
        (void)app_encoder_sync_sample(60200U, &out);
        (void)app_encoder_sync_sample(80200U, &out);
        CHECK(out.valid == false);
        CHECK(out.consecutive_fail >= 3U);

        /* 恢复：下一拍有效 */
        g_mock_enc_sync_fetch_fail[APP_ENCODER_ROTOR] = 0;
        CHECK(app_encoder_sync_sample(100200U, &out) == 0);
        CHECK(out.valid == true);
        CHECK(out.seq == 3U);
    }

    /* 6) 坏帧：raw 保持、seq 保持、jumped 置位 */
    g_mock_enc_raw[APP_ENCODER_ROTOR] = 40000U;
    CHECK(app_encoder_sync_sample(120200U, &out) == 1); /* HELD */
    CHECK(out.raw == 2010U);
    CHECK(out.seq == 3U);
    CHECK(out.jumped == true);

    /* 计数：每次 sync_sample（已 arm）恰好一次 fetch */
    CHECK(g_encoder_sample_count >= 7U);

    /* 重复 start：幂等（不重复 arm） */
    CHECK(app_encoder_sync_start() == 0);
    CHECK(g_mock_enc_sync_arm_count[APP_ENCODER_ROTOR] == 1);
}

void test_output_cache(void) {
    extern uint32_t g_mock_cycle;
    uint16_t raw = 123;
    CHECK(app_encoder_get_output_raw(&raw) != 0);
    CHECK(app_encoder_output_start() == 0);
    CHECK(g_mock_trgm_src == INTF_TRGM_SRC_PWM1_CH10REF);
    CHECK(g_mock_trgm_dst == INTF_TRGM_DST_DMA_REQ1);
    CHECK(app_encoder_read_raw(APP_ENCODER_OUTPUT, &raw) != 0);
    g_mock_enc_raw[APP_ENCODER_ROTOR] = 2010;
    app_encoder_rotor_snapshot_t rotor;
    CHECK(app_encoder_sync_sample(0, &rotor) == 0);
    g_mock_enc_raw[APP_ENCODER_OUTPUT] = 16384;
    CHECK(app_encoder_sync_sample(0, &rotor) == 0);
    CHECK(app_encoder_output_sample(&rotor, &raw) == 0);
    int reads = g_mock_enc_read_count[APP_ENCODER_OUTPUT];
    int fetches = g_mock_enc_sync_fetch_count[APP_ENCODER_OUTPUT];
    CHECK(app_encoder_get_output_raw(&raw) == 0);
    CHECK(raw == 16384);
    CHECK(g_mock_enc_read_count[APP_ENCODER_OUTPUT] == reads);
    CHECK(g_mock_enc_sync_fetch_count[APP_ENCODER_OUTPUT] == fetches);
    CHECK(app_encoder_output_sample(&rotor, &raw) == 0);
    CHECK(g_mock_enc_sync_fetch_count[APP_ENCODER_OUTPUT] == fetches);
    /* 未进入ADC发布前，不得把新的设备原码拼进旧快照。 */
    uint32_t pair_sequence = rotor.seq;
    g_mock_enc_raw[APP_ENCODER_ROTOR] = 2020;
    g_mock_enc_raw[APP_ENCODER_OUTPUT] = 20000;
    CHECK(app_encoder_output_sample(&rotor, &raw) == 0);
    CHECK(rotor.raw == 2010 && raw == 16384 && rotor.seq == pair_sequence);
    CHECK(app_encoder_sync_sample(1000, &rotor) == 0);
    g_mock_cycle = 1000;
    CHECK(app_encoder_output_sample(&rotor, &raw) == 0);
    CHECK(rotor.raw == 2020 && raw == 20000);
    CHECK(rotor.timestamp_cycles == 1000);
    CHECK(g_mock_enc_sync_fetch_count[APP_ENCODER_OUTPUT] == fetches + 1);
    g_mock_enc_sync_fetch_fail[APP_ENCODER_ROTOR] = 1;
    g_mock_enc_raw[APP_ENCODER_OUTPUT] = 32768;
    (void)app_encoder_sync_sample(20000, &rotor);
    CHECK(app_encoder_output_sample(&rotor, &raw) != 0);
    CHECK(app_encoder_get_output_raw(&raw) == 0);
    CHECK(raw == 32768); /* 原码观测独立于双编码器配对有效性 */
    g_mock_enc_sync_fetch_fail[APP_ENCODER_ROTOR] = 0;
    /* SPI1失败只使配对无效，不拒绝SPI3电流路径；旧缓存供诊断，不能供控制。 */
    g_mock_enc_sync_fetch_fail[APP_ENCODER_OUTPUT] = 1;
    CHECK(app_encoder_sync_sample(30000, &rotor) == 0);
    CHECK(rotor.valid);
    CHECK(app_encoder_output_sample(&rotor, &raw) != 0);
    CHECK(app_encoder_get_output_raw(&raw) == 0 && raw == 32768);
    CHECK(g_encoder_output_read_fail_count > 0);
    g_mock_enc_sync_fetch_fail[APP_ENCODER_OUTPUT] = 0;
    CHECK(app_encoder_sync_sample(40000, &rotor) == 0);
    g_mock_cycle = 40000;
    CHECK(app_encoder_output_sample(&rotor, &raw) == 0);
    CHECK(rotor.age_cycles == 0);
    CHECK(g_encoder_output_sample_count > 0);
    CHECK(g_encoder_pair_valid_count > 0);
    app_encoder_pair_stats_t stats;
    app_encoder_get_pair_stats(&stats);
    CHECK(stats.output_sample_count==g_encoder_output_sample_count);
    CHECK(stats.output_fail_count==g_encoder_output_read_fail_count);
    CHECK(stats.valid_pair_count==g_encoder_pair_valid_count);
    g_mock_cycle = 40000 + 19201; /* 超过一个25kHz周期，拒绝陈旧快照 */
    CHECK(app_encoder_output_sample(&rotor, &raw) != 0);
    /* 32bit mcycle回绕只改变数值，不影响配对年龄计算。 */
    CHECK(app_encoder_sync_sample(UINT32_MAX-100U, &rotor)==0);
    g_mock_cycle=99U;
    CHECK(app_encoder_output_sample(&rotor,&raw)==0);
    CHECK(rotor.age_cycles==200U);
    g_mock_cycle = 0;
}
