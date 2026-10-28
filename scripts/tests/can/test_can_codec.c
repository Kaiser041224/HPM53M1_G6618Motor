/** @file test_can_codec.c
 * @brief CAN 报文编解码测试（ID/控制帧/状态帧/MIT compact 与 dual）
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_can_protocol.h"
#include "test_util.h"

#include <string.h>

/** @brief 小端写入 f32（测试侧独立实现，避免与被测代码共用同一路径） */
static void t_put_f32(uint8_t* out, float v) {
    uint32_t u;

    memcpy(&u, &v, 4);
    out[0] = (uint8_t)(u & 0xFFU);
    out[1] = (uint8_t)((u >> 8) & 0xFFU);
    out[2] = (uint8_t)((u >> 16) & 0xFFU);
    out[3] = (uint8_t)((u >> 24) & 0xFFU);
}

/** @brief 小端写入 i16/u16 */
static void t_put_u16(uint8_t* out, uint16_t v) {
    out[0] = (uint8_t)(v & 0xFFU);
    out[1] = (uint8_t)(v >> 8);
}

void test_can_id(void) {
    CHECK(app_can_make_id(1U, APP_CAN_CMD_SET_INPUT_POS) == ((1U << 5) | 0x0CU));
    CHECK(app_can_make_id(15U, APP_CAN_CMD_HEARTBEAT) == ((15U << 5) | 0x01U));

    uint8_t node = 0U;
    uint8_t cmd = 0U;

    CHECK(app_can_parse_id(app_can_make_id(7U, APP_CAN_CMD_SET_LIMITS), &node, &cmd));
    CHECK(node == 7U);
    CHECK(cmd == APP_CAN_CMD_SET_LIMITS);
    CHECK(!app_can_parse_id(0x800U, &node, &cmd)); /* 超出 11 位 */
}

void test_can_decode_control(void) {
    app_can_frame_t f;
    uint8_t data[8] = {0};

    /* Estop：0 字节 */
    CHECK(app_can_decode(app_can_make_id(1U, APP_CAN_CMD_ESTOP), data, 0U, 1U, &f) == 0);
    CHECK(f.kind == APP_CAN_FRAME_ESTOP);

    /* Axis state */
    data[0] = APP_CAN_AXIS_CLOSED_LOOP;
    CHECK(app_can_decode(app_can_make_id(1U, APP_CAN_CMD_SET_AXIS_STATE), data, 1U, 1U, &f) == 0);
    CHECK(f.kind == APP_CAN_FRAME_AXIS_STATE);
    CHECK(f.u.axis_state == 8U);

    /* Controller mode */
    data[0] = APP_CAN_MODE_POSITION;
    data[1] = 1U;
    CHECK(app_can_decode(app_can_make_id(1U, APP_CAN_CMD_SET_CONTROLLER_MODE), data, 2U, 1U, &f)
          == 0);
    CHECK(f.u.mode.control_mode == APP_CAN_MODE_POSITION);

    /* Input pos：f32 90deg + i16 0.001rpm + i16 0.001Nm */
    memset(data, 0, sizeof(data));
    t_put_f32(&data[0], 90.0f);
    t_put_u16(&data[4], (uint16_t)(int16_t)1200);  /* 1.2 rpm */
    t_put_u16(&data[6], (uint16_t)(int16_t)(-500)); /* -0.5 Nm */
    CHECK(app_can_decode(app_can_make_id(1U, APP_CAN_CMD_SET_INPUT_POS), data, 8U, 1U, &f) == 0);
    CHECK(f.kind == APP_CAN_FRAME_INPUT_POS);
    CHECK_NEAR(f.u.pos.pos_deg, 90.0f, 1e-5f);
    CHECK_NEAR(f.u.pos.vel_ff_rpm, 1.2f, 1e-4f);
    CHECK_NEAR(f.u.pos.torque_ff_nm, -0.5f, 1e-4f);

    /* 长度不足 */
    CHECK(app_can_decode(app_can_make_id(1U, APP_CAN_CMD_SET_INPUT_POS), data, 7U, 1U, &f) == -1);

    /* 非本节点：返回 1（不是错误） */
    CHECK(app_can_decode(app_can_make_id(2U, APP_CAN_CMD_SET_INPUT_POS), data, 8U, 1U, &f) == 1);

    /* 未知命令 */
    CHECK(app_can_decode(app_can_make_id(1U, 0x1AU), data, 8U, 1U, &f) == 1);
    CHECK(f.kind == APP_CAN_FRAME_UNKNOWN);

    /* Limits */
    t_put_f32(&data[0], 1500.0f);
    t_put_f32(&data[4], 20.0f);
    CHECK(app_can_decode(app_can_make_id(1U, APP_CAN_CMD_SET_LIMITS), data, 8U, 1U, &f) == 0);
    CHECK_NEAR(f.u.limits.vel_limit_rpm, 1500.0f, 1e-4f);
    CHECK_NEAR(f.u.limits.current_limit_a, 20.0f, 1e-4f);

    /* Clear errors */
    data[0] = 0x02U;
    data[1] = 0x00U;
    data[2] = 0x00U;
    data[3] = 0x00U;
    CHECK(app_can_decode(app_can_make_id(1U, APP_CAN_CMD_CLEAR_ERRORS), data, 4U, 1U, &f) == 0);
    CHECK(f.u.identify == 0x02U);
}

void test_can_encode_status(void) {
    uint8_t data[8];

    memset(data, 0xFF, sizeof(data));
    CHECK(app_can_encode_heartbeat(data, 0x00000201U, 8U, 3U, 1U) == 8U);
    CHECK(data[0] == 0x01U);
    CHECK(data[1] == 0x02U);
    CHECK(data[2] == 0x00U);
    CHECK(data[3] == 0x00U);
    CHECK(data[4] == 8U);
    CHECK(data[5] == 3U);
    CHECK(data[6] == 1U);
    CHECK(data[7] == 0U);

    CHECK(app_can_encode_encoder(data, 12.5f, -30.0f) == 8U);
    {
        uint32_t u;
        float v;

        memcpy(&u, &data[0], 4);
        memcpy(&v, &u, 4);
        CHECK_NEAR(v, 12.5f, 1e-6f);
        memcpy(&u, &data[4], 4);
        memcpy(&v, &u, 4);
        CHECK_NEAR(v, -30.0f, 1e-6f);
    }

    CHECK(app_can_encode_error(data, 0x11U, 0x22U) == 8U);
    CHECK(data[0] == 0x11U);
    CHECK(data[4] == 0x22U);
}

void test_can_mit_compact(void) {
    uint8_t data[8];
    app_can_frame_t f;
    float kp_max = 50.0f;
    float kd_max = 2.0f;

    CHECK(app_can_encode_mit(data, 90.0f, 1500.0f, 25.0f, 1.0f, 1.23f, kp_max, kd_max) == 8U);
    CHECK(app_can_decode(app_can_make_id(1U, APP_CAN_CMD_SET_INPUT_MIT), data, 8U, 1U, &f) == 0);
    CHECK(f.kind == APP_CAN_FRAME_MIT);
    CHECK_NEAR(f.u.mit.p_deg, 90.0f, 0.11f);   /* 0.1 deg 定标 */
    CHECK_NEAR(f.u.mit.v_rpm, 1500.0f, 1.0f);  /* 1 rpm 定标 */
    CHECK_NEAR(f.u.mit.kp, 0.5f, 1e-3f);       /* 归一化 */
    CHECK_NEAR(f.u.mit.kd, 0.5f, 1e-3f);
    CHECK_NEAR(f.u.mit.t_ff, 1.23f, 0.011f);   /* 0.01 Nm 定标 */

    /* 负值 */
    CHECK(app_can_encode_mit(data, -270.0f, -800.0f, 0.0f, 0.0f, -2.5f, kp_max, kd_max) == 8U);
    CHECK(app_can_decode(app_can_make_id(1U, APP_CAN_CMD_SET_INPUT_MIT), data, 8U, 1U, &f) == 0);
    CHECK_NEAR(f.u.mit.p_deg, -270.0f, 0.11f);
    CHECK_NEAR(f.u.mit.v_rpm, -800.0f, 1.0f);
    CHECK_NEAR(f.u.mit.t_ff, -2.5f, 0.011f);

    /* 饱和：p 超 ±3276.7deg、v 超 ±2047rpm */
    CHECK(app_can_encode_mit(data, 9000.0f, 5000.0f, 0.0f, 0.0f, 0.0f, kp_max, kd_max) == 8U);
    CHECK(app_can_decode(app_can_make_id(1U, APP_CAN_CMD_SET_INPUT_MIT), data, 8U, 1U, &f) == 0);
    CHECK_NEAR(f.u.mit.p_deg, 3276.7f, 0.2f);
    CHECK_NEAR(f.u.mit.v_rpm, 2047.0f, 1.0f);

    /* 长度不足 */
    CHECK(app_can_decode(app_can_make_id(1U, APP_CAN_CMD_SET_INPUT_MIT), data, 7U, 1U, &f) == -1);
}

void test_can_mit_dual(void) {
    uint8_t a[8];
    uint8_t b[8];
    app_can_mit_a_t pa;
    app_can_mit_b_t pb;

    t_put_f32(&a[0], 123.456f);
    t_put_u16(&a[4], (uint16_t)(int16_t)12000); /* 1200.0 rpm */
    a[6] = 7U;
    a[7] = 0U;

    CHECK(app_can_decode_mit_a(a, 8U, &pa) == 0);
    CHECK_NEAR(pa.p_deg, 123.456f, 1e-5f);
    CHECK_NEAR(pa.v_rpm, 1200.0f, 0.05f);
    CHECK(pa.seq == 7U);

    t_put_u16(&b[0], 25000U); /* kp = 25.000 */
    t_put_u16(&b[2], 1500U);  /* kd = 1.500 */
    t_put_u16(&b[4], (uint16_t)(int16_t)(-1200)); /* t_ff = -1.200 */
    b[6] = 7U;
    b[7] = 0U;

    CHECK(app_can_decode_mit_b(b, 8U, &pb) == 0);
    CHECK_NEAR(pb.kp, 25.0f, 1e-4f);
    CHECK_NEAR(pb.kd, 1.5f, 1e-4f);
    CHECK_NEAR(pb.t_ff, -1.2f, 1e-4f);
    CHECK(pb.seq == 7U);

    /* 长度不足 */
    CHECK(app_can_decode_mit_a(a, 7U, &pa) == -1);
    CHECK(app_can_decode_mit_b(b, 7U, &pb) == -1);
}