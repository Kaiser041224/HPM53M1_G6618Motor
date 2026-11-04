/**
 * @file    test_can_codec.c
 * @brief   DM 兼容 CAN 协议纯编解码自测（ID/控制帧/反馈帧/定标/ERR 映射）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_can_protocol.h"
#include "test_util.h"

#include <string.h>

/** @brief 小端写入 f32（测试侧独立实现） */
static void t_put_f32(uint8_t* out, float v) {
    uint32_t u;

    memcpy(&u, &v, 4);
    out[0] = (uint8_t)(u & 0xFFU);
    out[1] = (uint8_t)((u >> 8) & 0xFFU);
    out[2] = (uint8_t)((u >> 16) & 0xFFU);
    out[3] = (uint8_t)((u >> 24) & 0xFFU);
}

/** @brief 小端写入 u16 */
static void t_put_u16(uint8_t* out, uint16_t v) {
    out[0] = (uint8_t)(v & 0xFFU);
    out[1] = (uint8_t)(v >> 8);
}

/* ── ID ─────────────────────────────────────────────────────────────── */

void test_can_dm_id(void) {
    uint8_t type = 0U;
    uint8_t node = 0U;

    CHECK(app_can_dm_make_id(APP_CAN_DM_TYPE_MIT, 1U) == 0x111U);
    CHECK(app_can_dm_feedback_id(7U) == 0x187U);
    CHECK(app_can_dm_make_id(APP_CAN_DM_TYPE_CLEAR, 15U) == 0x17FU);

    CHECK(app_can_dm_parse_id(0x111U, &type, &node));
    CHECK(type == APP_CAN_DM_TYPE_MIT && node == 1U);
    CHECK(app_can_dm_parse_id(0x187U, &type, &node));
    CHECK(type == APP_CAN_DM_TYPE_FEEDBACK && node == 7U);
    CHECK(app_can_dm_parse_id(0x11FU, &type, &node));
    CHECK(node == 15U);

    /* 类型 0 / 9..15 / 非 0x1XX / 超过 11 位 一律不匹配 */
    CHECK(!app_can_dm_parse_id(0x100U, &type, &node));
    CHECK(!app_can_dm_parse_id(0x190U, &type, &node));
    CHECK(!app_can_dm_parse_id(0x1A1U, &type, &node));
    CHECK(!app_can_dm_parse_id(0x211U, &type, &node));
    CHECK(!app_can_dm_parse_id(0x800U, &type, &node));
}

/* ── MIT ────────────────────────────────────────────────────────────── */

void test_can_dm_mit(void) {
    /* 手册位打包向量：p=0x7FFF, v=kp=kd=t=0x7FF */
    const uint8_t mit[8] = {0x7F, 0xFF, 0x7F, 0xF7, 0xFF, 0x7F, 0xF7, 0xFF};
    app_can_dm_frame_t f;

    CHECK(app_can_dm_decode(0x111U, mit, 8U, 1U, NULL, &f) == 0);
    CHECK(f.kind == APP_CAN_DM_KIND_MIT);
    CHECK_NEAR(f.p_des_rad, 0.0f, 0.01f);
    CHECK_NEAR(f.v_des_rad_s, 0.0f, 0.02f);
    CHECK_NEAR(f.kp, 249.94f, 0.1f);
    CHECK_NEAR(f.kd, 2.4994f, 0.01f);
    CHECK_NEAR(f.t_ff_nm, 0.0f, 0.01f);

    /* 长度不足 / 非本机 / 未知类型 */
    CHECK(app_can_dm_decode(0x111U, mit, 7U, 1U, NULL, &f) == -1);
    CHECK(app_can_dm_decode(0x111U, mit, 8U, 2U, NULL, &f) == 1);
    CHECK(app_can_dm_decode(0x191U, mit, 8U, 1U, NULL, &f) == 1);
}

/* ── 位置速度 / 速度 / 力位混控 ─────────────────────────────────────── */

void test_can_dm_control_float(void) {
    uint8_t data[8];
    app_can_dm_frame_t f;

    memset(data, 0, sizeof(data));
    t_put_f32(&data[0], 1.5f);
    t_put_f32(&data[4], -2.25f);
    CHECK(app_can_dm_decode(0x121U, data, 8U, 1U, NULL, &f) == 0);
    CHECK(f.kind == APP_CAN_DM_KIND_POS_VEL);
    CHECK_NEAR(f.p_des_rad, 1.5f, 1e-6f);
    CHECK_NEAR(f.v_des_rad_s, -2.25f, 1e-6f);

    /* 速度帧：DLC 4..8 均可，只取前 4 字节 */
    t_put_f32(&data[0], 3.25f);
    CHECK(app_can_dm_decode(0x131U, data, 4U, 1U, NULL, &f) == 0);
    CHECK(f.kind == APP_CAN_DM_KIND_VEL);
    CHECK_NEAR(f.v_des_rad_s, 3.25f, 1e-6f);
    CHECK(app_can_dm_decode(0x131U, data, 8U, 1U, NULL, &f) == 0);
    CHECK(app_can_dm_decode(0x131U, data, 3U, 1U, NULL, &f) == -1);

    /* 力位混控：p=f32、v=u16/100、i=u16/10000 */
    t_put_f32(&data[0], 0.5f);
    t_put_u16(&data[4], 1234U);
    t_put_u16(&data[6], 5000U);
    CHECK(app_can_dm_decode(0x141U, data, 8U, 1U, NULL, &f) == 0);
    CHECK(f.kind == APP_CAN_DM_KIND_FORCE_POS);
    CHECK_NEAR(f.p_des_rad, 0.5f, 1e-6f);
    CHECK_NEAR(f.vel_limit_rad_s, 12.34f, 1e-4f);
    CHECK_NEAR(f.iq_ratio, 0.5f, 1e-6f);

    t_put_u16(&data[6], 20000U); /* >10000 钳到 1.0 */
    CHECK(app_can_dm_decode(0x141U, data, 8U, 1U, NULL, &f) == 0);
    CHECK_NEAR(f.iq_ratio, 1.0f, 1e-6f);
}

void test_can_dm_control_nan(void) {
    uint8_t data[8];
    app_can_dm_frame_t f;

    memset(data, 0, sizeof(data));
    data[0] = 0x00;
    data[1] = 0x00;
    data[2] = 0xC0;
    data[3] = 0x7F; /* 小端 0x7FC00000 = NaN */
    CHECK(app_can_dm_decode(0x121U, data, 8U, 1U, NULL, &f) == -1);

    data[0] = 0x00;
    data[1] = 0x00;
    data[2] = 0x80;
    data[3] = 0x7F; /* +Inf */
    CHECK(app_can_dm_decode(0x131U, data, 4U, 1U, NULL, &f) == -1);

    data[0] = 0x00;
    data[1] = 0x00;
    data[2] = 0x80;
    data[3] = 0x7F;
    CHECK(app_can_dm_decode(0x141U, data, 8U, 1U, NULL, &f) == -1);
}

/* ── 使能 / 失能 / 清故障 ───────────────────────────────────────────── */

void test_can_dm_status_frames(void) {
    uint8_t data[8] = {0};
    app_can_dm_frame_t f;

    data[0] = APP_CAN_DM_MAGIC_ENABLE;
    CHECK(app_can_dm_decode(0x151U, data, 1U, 1U, NULL, &f) == 0);
    CHECK(f.kind == APP_CAN_DM_KIND_ENABLE);

    data[0] = APP_CAN_DM_MAGIC_DISABLE;
    CHECK(app_can_dm_decode(0x161U, data, 8U, 1U, NULL, &f) == 0);
    CHECK(f.kind == APP_CAN_DM_KIND_DISABLE);

    data[0] = APP_CAN_DM_MAGIC_CLEAR;
    CHECK(app_can_dm_decode(0x171U, data, 1U, 1U, NULL, &f) == 0);
    CHECK(f.kind == APP_CAN_DM_KIND_CLEAR);

    /* magic 错误 / DLC 0 */
    data[0] = 0x00U;
    CHECK(app_can_dm_decode(0x151U, data, 1U, 1U, NULL, &f) == -1);
    data[0] = APP_CAN_DM_MAGIC_ENABLE;
    CHECK(app_can_dm_decode(0x151U, data, 0U, 1U, NULL, &f) == -1);

    /* 其他节点的使能帧不执行 */
    CHECK(app_can_dm_decode(0x152U, data, 1U, 1U, NULL, &f) == 1);
}

/* ── 反馈帧 ─────────────────────────────────────────────────────────── */

void test_can_dm_feedback(void) {
    uint8_t out[8] = {0};

    /* node=5 err=1 pos=0 vel=0 t=0 t_mos=40 t_rotor=50 */
    CHECK(app_can_dm_encode_feedback(out, 5U, 1U, 0.0f, 0.0f, 0.0f, 40U, 50U, NULL) == 8U);
    CHECK(out[0] == 0x15U);
    CHECK(out[1] == 0x7FU);
    CHECK(out[2] == 0xFFU);
    CHECK(out[3] == 0x7FU);
    CHECK(out[4] == 0xF7U);
    CHECK(out[5] == 0xFFU);
    CHECK(out[6] == 40U);
    CHECK(out[7] == 50U);

    /* pos=1.0, vel=2.0, t=1.0 → raw 35388/2184/2057（T 按反馈量程 ±200） */
    CHECK(app_can_dm_encode_feedback(out, 1U, 1U, 1.0f, 2.0f, 1.0f, 0U, 0U, NULL) == 8U);
    CHECK(out[0] == 0x11U);
    CHECK(out[1] == 0x8AU);
    CHECK(out[2] == 0x3CU);
    CHECK(out[3] == 0x88U);
    CHECK(out[4] == 0x88U);
    CHECK(out[5] == 0x09U);

    /* 越界饱和 */
    CHECK(app_can_dm_encode_feedback(out, 1U, 1U, 1e9f, 1e9f, 1e9f, 0U, 0U, NULL) == 8U);
    CHECK(out[1] == 0xFFU && out[2] == 0xFFU);
    CHECK(out[3] == 0xFFU && out[4] >= 0xF0U);
    CHECK(app_can_dm_encode_feedback(out, 1U, 1U, -1e9f, -1e9f, -1e9f, 0U, 0U, NULL) == 8U);
    CHECK(out[1] == 0x00U && out[2] == 0x00U);
    CHECK(out[3] == 0x00U && out[4] == 0x00U && out[5] == 0x00U);
}

/* ── ERR 映射与定标 ─────────────────────────────────────────────────── */

void test_can_dm_scaling(void) {
    float span = APP_CAN_DM_P_MAX_RAD * 2.0f;

    CHECK_NEAR(app_can_dm_uint_to_float(0U, -APP_CAN_DM_P_MAX_RAD, APP_CAN_DM_P_MAX_RAD, 16U),
               -APP_CAN_DM_P_MAX_RAD, 1e-6f);
    CHECK_NEAR(app_can_dm_uint_to_float(65535U, -APP_CAN_DM_P_MAX_RAD, APP_CAN_DM_P_MAX_RAD, 16U),
               APP_CAN_DM_P_MAX_RAD, 1e-6f);
    CHECK(app_can_dm_float_to_uint(APP_CAN_DM_P_MAX_RAD, -APP_CAN_DM_P_MAX_RAD,
                                   APP_CAN_DM_P_MAX_RAD, 16U)
          == 65535U);
    CHECK(app_can_dm_float_to_uint(-APP_CAN_DM_P_MAX_RAD, -APP_CAN_DM_P_MAX_RAD,
                                   APP_CAN_DM_P_MAX_RAD, 16U)
          == 0U);
    CHECK(app_can_dm_float_to_uint(1e9f, -APP_CAN_DM_P_MAX_RAD, APP_CAN_DM_P_MAX_RAD, 16U)
          == 65535U);

    /* 往返误差 ≤ 1 LSB（16 位：span/65535） */
    const float values[] = {-12.0f, -1.25f, 0.0f, 0.5f, 3.14159f, 12.4f};

    for (unsigned i = 0; i < (sizeof(values) / sizeof(values[0])); i++) {
        uint32_t raw = app_can_dm_float_to_uint(values[i], -APP_CAN_DM_P_MAX_RAD,
                                                APP_CAN_DM_P_MAX_RAD, 16U);
        float back = app_can_dm_uint_to_float(raw, -APP_CAN_DM_P_MAX_RAD,
                                              APP_CAN_DM_P_MAX_RAD, 16U);

        CHECK_NEAR(back, values[i], (span / 65535.0f) + 1e-6f);
    }
}

/* MIT t_ff 与反馈 T 的量程分离：t_ff 保持 ±10 Nm，反馈 T 用 ±200 Nm */
void test_can_dm_torque_scale_split(void) {
    const uint8_t t_max[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xFF};
    app_can_dm_frame_t f;
    uint8_t out[8] = {0};

    /* MIT t_ff 满码仍解出 +10 Nm（DM 兼容） */
    CHECK(app_can_dm_decode(0x110U, t_max, 8U, 0U, NULL, &f) == 0);
    CHECK_NEAR(f.t_ff_nm, 10.0f, 1e-3f);

    /* 反馈 T 满量程 ±200 Nm */
    CHECK(app_can_dm_encode_feedback(out, 0U, 1U, 0.0f, 0.0f, 200.0f, 0U, 0U, NULL) == 8U);
    CHECK(out[4] == 0xFFU && out[5] == 0xFFU);
    CHECK(app_can_dm_encode_feedback(out, 0U, 1U, 0.0f, 0.0f, -200.0f, 0U, 0U, NULL) == 8U);
    CHECK(out[4] == 0xF0U && out[5] == 0x00U);

    /* 142 Nm（24.3 A 连续）不再被 10 Nm 截断 */
    CHECK(app_can_dm_encode_feedback(out, 0U, 1U, 0.0f, 0.0f, 142.0f, 0U, 0U, NULL) == 8U);
    CHECK(out[4] == 0xFDU && out[5] == 0xADU);
}
