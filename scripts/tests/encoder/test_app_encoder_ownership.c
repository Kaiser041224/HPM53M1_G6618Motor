/**
 * @file    test_app_encoder_ownership.c
 * @brief   app_encoder 单一所有者 + 快照发布 主机自测（mock SPI/编码器）
 * @author  Kaiser
 *
 * 覆盖：
 *   1) 采样器 claim 前，read_raw 走物理设备
 *   2) 采样器 claim 后，运行期 read_raw 只读快照、不触发设备读
 *   3) 坏帧不污染已发布 raw、序号不推进
 *   4) 快照携带时间戳与性别
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

#include "app_encoder.h"

extern void mock_reset(void);
extern int g_mock_enc_read_count[2];
extern int g_mock_enc_read_reg_count[2];
extern uint16_t g_mock_enc_raw[2];
extern uint32_t g_mock_cycle;

void test_app_encoder_ownership(void) {
    uint16_t raw = 0U;
    int reads_before;
    int reads_after;
    app_encoder_rotor_snapshot_t snap;

    mock_reset();
    g_mock_enc_raw[APP_ENCODER_ROTOR] = 1000U;
    g_mock_enc_raw[APP_ENCODER_OUTPUT] = 42U;

    CHECK(app_encoder_init() == 0);

    /* 1) claim 前：read_raw 触发一次设备读 */
    reads_before = g_mock_enc_read_count[APP_ENCODER_ROTOR];
    CHECK(app_encoder_read_raw(APP_ENCODER_ROTOR, &raw) == 0);
    CHECK(raw == 1000U);
    CHECK(g_mock_enc_read_count[APP_ENCODER_ROTOR] == reads_before + 1);

    /* 2) 声明采样器所有权：此后运行期读只走快照 */
    app_encoder_sampler_claim();
    g_mock_cycle = 1000U;
    CHECK(app_encoder_sample_rotor_at(g_mock_cycle) == 0);

    reads_after = g_mock_enc_read_count[APP_ENCODER_ROTOR];
    CHECK(app_encoder_read_raw(APP_ENCODER_ROTOR, &raw) == 0);
    CHECK(raw == 1000U);
    CHECK(g_mock_enc_read_count[APP_ENCODER_ROTOR] == reads_after); /* 未触发 SPI */

    CHECK(app_encoder_get_rotor_snapshot(&snap) == 0);
    CHECK(snap.raw == 1000U);
    CHECK(snap.valid == true);
    CHECK(snap.seq == 1U);
    CHECK(snap.timestamp_cycles == 1000U);

    /* 3) 坏帧：发布值保持、序号保持 */
    g_mock_enc_raw[APP_ENCODER_ROTOR] = 30000U;
    g_mock_cycle = 2000U;
    (void)app_encoder_sample_rotor_at(g_mock_cycle);
    CHECK(app_encoder_get_rotor_snapshot(&snap) == 0);
    CHECK(snap.raw == 1000U);
    CHECK(snap.seq == 1U);
    CHECK(snap.jumped == true);

    /* 4) 运行期读仍不触发设备 */
    reads_after = g_mock_enc_read_count[APP_ENCODER_ROTOR];
    CHECK(app_encoder_read_raw(APP_ENCODER_ROTOR, &raw) == 0);
    CHECK(g_mock_enc_read_count[APP_ENCODER_ROTOR] == reads_after);

    /* 5) 运行期寄存器读/MTP 写/方向写：转子被拒绝且不触发 SPI；出轴允许 */
    {
        uint8_t v = 0U;
        int regs_before = g_mock_enc_read_reg_count[APP_ENCODER_ROTOR];

        CHECK(app_encoder_read_reg(APP_ENCODER_ROTOR, 0x09U, &v) == -1);
        CHECK(g_mock_enc_read_reg_count[APP_ENCODER_ROTOR] == regs_before); /* 未碰 SPI */
        CHECK(app_encoder_set_direction(APP_ENCODER_ROTOR, true) == -1);
        CHECK(app_encoder_set_zero_mtp(APP_ENCODER_ROTOR, 0U) == -1);
        CHECK(app_encoder_read_reg(APP_ENCODER_OUTPUT, 0x09U, &v) == 0); /* 出轴不受限 */
    }
}
