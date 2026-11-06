/**
 * @file    test_usb_proto.c
 * @brief   USB bulk 协议服务主机自测：帧层 / 分发 / 参数 / 控制 / 遥测流
 * @author  Kaiser
 *
 * 覆盖：
 *   - 金向量请求、坏 CRC、超长 LEN、垃圾重同步、半帧跨调用、单块多帧；
 *   - PARAM_READ/WRITE/COMMIT/RESET 的状态码、范围、类型、apply 与编码器标志；
 *   - CTRL START/STOP/MODE/TORQUE/SPEED/ANGLE 的映射与 motion 返回码映射；
 *   - STREAM_SUB/UNSUB：batch 累积、sample_index 连续、未就绪丢批、范围校验；
 *   - 应答端点忙时的 pending 重发。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_usb_proto.h"

#include "app_hardware_params.h"
#include "app_motor_params.h"
#include "app_param_meta.h"
#include "app_software_params.h"
#include "motor_proto.h"
#include "mock_deps.h"
#include "test_util.h"

#include <math.h>
#include <string.h>

extern bool g_mock_flash_fail_store;

#define SOF0 (0xA5U)
#define SOF1 (0x5AU)
#define VER (0x01U)

/* ============================================================================
 * 工具
 * ============================================================================ */

static uint16_t crc16_update(uint16_t crc, uint8_t byte) {
    crc ^= (uint16_t) ((uint16_t) byte << 8);
    for (uint8_t i = 0U; i < 8U; i++) {
        crc = ((crc & 0x8000U) != 0U) ? (uint16_t) ((crc << 1) ^ 0x1021U)
                                      : (uint16_t) (crc << 1);
    }
    return crc;
}

static uint16_t u16_at(const uint8_t *p, uint16_t off) {
    return (uint16_t) (p[off] | ((uint16_t) p[off + 1U] << 8));
}

static uint32_t u32_at(const uint8_t *p, uint16_t off) {
    return (uint32_t) p[off] | ((uint32_t) p[off + 1U] << 8) | ((uint32_t) p[off + 2U] << 16)
           | ((uint32_t) p[off + 3U] << 24);
}

static float f32_at(const uint8_t *p, uint16_t off) {
    union {
        float f;
        uint32_t u;
    } bits;

    bits.u = u32_at(p, off);
    return bits.f;
}

static void put_f32(uint8_t *p, float v) {
    union {
        float f;
        uint32_t u;
    } bits;

    bits.f = v;
    p[0] = (uint8_t) (bits.u & 0xFFU);
    p[1] = (uint8_t) ((bits.u >> 8) & 0xFFU);
    p[2] = (uint8_t) ((bits.u >> 16) & 0xFFU);
    p[3] = (uint8_t) ((bits.u >> 24) & 0xFFU);
}

static uint16_t build_req(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint16_t len,
                          uint8_t *out) {
    uint16_t crc = 0xFFFFU;

    out[0] = SOF0;
    out[1] = SOF1;
    out[2] = VER;
    out[3] = cmd;
    out[4] = seq;
    out[5] = (uint8_t) (len & 0xFFU);
    out[6] = (uint8_t) ((len >> 8) & 0xFFU);
    if (len > 0U) {
        memcpy(&out[7], payload, len);
    }
    for (uint16_t i = 2U; i < (uint16_t) (7U + len); i++) {
        crc = crc16_update(crc, out[i]);
    }
    out[7U + len] = (uint8_t) (crc & 0xFFU);
    out[8U + len] = (uint8_t) ((crc >> 8) & 0xFFU);
    return (uint16_t) (9U + len);
}

static void feed(const uint8_t *data, uint16_t len) {
    memcpy(&g_mock.rx[g_mock.rx_len], data, len);
    g_mock.rx_len = (uint16_t) (g_mock.rx_len + len);
}

/** 清空 IO 与模块状态，并把三个参数域置为出厂默认。 */
static void reset_all(void) {
    mock_reset();
    *app_motor_params_mutable() = *app_motor_params_default();
    *app_software_params_mutable() = *app_software_params_default();
    *app_hardware_params_mutable() = *app_hardware_params_default();
    app_usb_proto_init();
}

/** 消费接收缓冲（有界），期间可能发送应答/流帧。 */
static void run_service(void) {
    for (int guard = 0; (guard < 32) && (g_mock.rx_pos < g_mock.rx_len); guard++) {
        app_usb_proto_service();
    }
}

/** 解析最近一次写出的应答帧，返回 payload 副本。 */
static const uint8_t *last_payload(uint8_t cmd, uint8_t seq, uint16_t *plen) {
    static uint8_t copy[600];
    uint16_t len;
    uint16_t crc = 0xFFFFU;

    CHECK(g_mock.tx_count > 0);
    CHECK(g_mock.tx_len >= 9U);
    CHECK(g_mock.tx[0] == SOF0);
    CHECK(g_mock.tx[1] == SOF1);
    CHECK(g_mock.tx[2] == VER);
    CHECK(g_mock.tx[3] == (uint8_t) (cmd | MP_RESPONSE_BIT));
    CHECK(g_mock.tx[4] == seq);

    len = u16_at(g_mock.tx, 5);
    CHECK(len <= MP_MAX_PAYLOAD);
    CHECK(g_mock.tx_len == (uint16_t) (9U + len));
    for (uint16_t i = 2U; i < (uint16_t) (7U + len); i++) {
        crc = crc16_update(crc, g_mock.tx[i]);
    }
    CHECK(crc == u16_at(g_mock.tx, (uint16_t) (7U + len)));

    memcpy(copy, &g_mock.tx[7], len);
    *plen = len;
    return copy;
}

/** 一次完整事务：喂请求 → 消费 → 校验应答；返回 status（0xFF = 无应答）。 */
static uint8_t transact(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint16_t len,
                        const uint8_t **resp, uint16_t *resp_len) {
    uint8_t frame[600];
    uint16_t flen = build_req(cmd, seq, payload, len, frame);

    g_mock.rx_len = 0U;
    g_mock.rx_pos = 0U;
    g_mock.tx_count = 0;
    g_mock.tx_len = 0U;
    feed(frame, flen);
    run_service();

    if (g_mock.tx_count == 0) {
        *resp = NULL;
        *resp_len = 0U;
        return 0xFFU;
    }
    *resp = last_payload(cmd, seq, resp_len);
    return (*resp)[0];
}

static uint16_t build_param_write(uint8_t *out, const char *name, uint8_t type, uint32_t raw) {
    size_t n = strlen(name);
    uint16_t pos = 0U;

    out[pos++] = (uint8_t) n;
    memcpy(&out[pos], name, n);
    pos = (uint16_t) (pos + n);
    out[pos++] = type;
    out[pos++] = (uint8_t) (raw & 0xFFU);
    out[pos++] = (uint8_t) ((raw >> 8) & 0xFFU);
    out[pos++] = (uint8_t) ((raw >> 16) & 0xFFU);
    out[pos++] = (uint8_t) ((raw >> 24) & 0xFFU);
    return pos;
}

static uint16_t build_param_read(uint8_t *out, const char *name) {
    size_t n = strlen(name);

    out[0] = (uint8_t) n;
    memcpy(&out[1], name, n);
    return (uint16_t) (1U + n);
}

static uint16_t build_reset(uint8_t *out, const char *const *names, uint8_t count) {
    uint16_t pos = 1U;

    out[0] = count;
    for (uint8_t i = 0U; i < count; i++) {
        size_t n = strlen(names[i]);
        out[pos++] = (uint8_t) n;
        memcpy(&out[pos], names[i], n);
        pos = (uint16_t) (pos + n);
    }
    return pos;
}

static uint16_t ctrl_req(uint8_t op, float value, uint8_t *out) {
    out[0] = op;
    put_f32(&out[1], value);
    return 5U;
}

/* ============================================================================
 * 帧层
 * ============================================================================ */

/** 金向量 PROTO_INFO 请求被接受；应答字段与 caps 正确。 */
static void test_info_golden(void) {
    static const uint8_t golden[] = {0xA5, 0x5A, 0x01, 0x01, 0x00, 0x00, 0x00, 0xE9, 0xCD};
    const uint8_t *resp;
    uint16_t len;

    reset_all();
    feed(golden, sizeof(golden));
    run_service();
    CHECK(g_mock.tx_count == 1);

    resp = last_payload(MP_CMD_PROTO_INFO, 0U, &len);
    CHECK(len == 15U); /* status 1 + major 1 + minor 1 + caps 4 + name_len 1 + name 7 */
    CHECK(resp[0] == MP_STATUS_OK);
    CHECK(resp[1] == 1U);
    CHECK(resp[2] == 0U);
    CHECK(u32_at(resp, 3) == (uint32_t) (MP_CAP_PARAM_WRITE | MP_CAP_CONTROL | MP_CAP_STREAM));
    CHECK(resp[7] == 7U);
    CHECK(memcmp(&resp[8], "HPM53M1", 7U) == 0);
}

/** 坏 CRC 被丢弃；随后合法帧仍能同步。 */
static void test_bad_crc_resync(void) {
    uint8_t frame[64];
    uint16_t flen;

    reset_all();
    flen = build_req(MP_CMD_PROTO_INFO, 3U, NULL, 0U, frame);
    frame[flen - 1U] ^= 0xFFU; /* 破坏 CRC 高字节 */

    feed(frame, flen);
    run_service();
    CHECK(g_mock.tx_count == 0);

    flen = build_req(MP_CMD_PROTO_INFO, 4U, NULL, 0U, frame);
    feed(frame, flen);
    run_service();
    CHECK(g_mock.tx_count == 1);
}

/** LEN > MP_MAX_PAYLOAD 被丢弃；随后合法帧仍能同步。 */
static void test_oversize_len(void) {
    uint8_t bad[9] = {SOF0, SOF1, VER, MP_CMD_PROTO_INFO, 0U, 0x58U, 0x02U, 0U, 0U};
    uint8_t frame[64];
    uint16_t flen;

    reset_all();
    feed(bad, sizeof(bad)); /* LEN = 600 */
    run_service();
    CHECK(g_mock.tx_count == 0);

    flen = build_req(MP_CMD_PROTO_INFO, 5U, NULL, 0U, frame);
    feed(frame, flen);
    run_service();
    CHECK(g_mock.tx_count == 1);
}

/** 垃圾字节后仍能重同步。 */
static void test_garbage_resync(void) {
    static const uint8_t junk[] = {0x00, 0xFF, 0xA5, 0x00, 0x12, 0xA5, 0x5A, 0x02};
    uint8_t frame[64];
    uint16_t flen;

    reset_all();
    feed(junk, sizeof(junk));
    run_service();
    CHECK(g_mock.tx_count == 0);

    flen = build_req(MP_CMD_PROTO_INFO, 6U, NULL, 0U, frame);
    feed(frame, flen);
    run_service();
    CHECK(g_mock.tx_count == 1);
}

/** 半帧跨多次 service()：字节未齐时不应答，齐了恰好一帧。 */
static void test_partial_frame(void) {
    uint8_t frame[64];
    uint16_t flen;

    reset_all();
    flen = build_req(MP_CMD_PROTO_INFO, 7U, NULL, 0U, frame);

    for (uint16_t i = 0U; i < flen; i++) {
        g_mock.rx_pos = i;
        g_mock.rx[i] = frame[i];
        g_mock.rx_len = (uint16_t) (i + 1U);
        app_usb_proto_service();
        if ((i + 1U) < flen) {
            CHECK(g_mock.tx_count == 0);
        }
    }
    CHECK(g_mock.tx_count == 1);
}

/** 同一块内两帧：两条应答都发出。 */
static void test_two_requests_one_chunk(void) {
    uint8_t frame[64];
    uint16_t flen;

    reset_all();
    flen = build_req(MP_CMD_PROTO_INFO, 8U, NULL, 0U, frame);
    feed(frame, flen);
    flen = build_req(MP_CMD_PROTO_INFO, 9U, NULL, 0U, frame);
    feed(frame, flen);
    run_service();

    CHECK(g_mock.tx_count == 2);
    (void) last_payload(MP_CMD_PROTO_INFO, 9U, &flen);
}

/* ============================================================================
 * 参数
 * ============================================================================ */

static void test_param_read(void) {
    const uint8_t *resp;
    uint8_t req[64];
    uint16_t len;
    uint16_t rlen;

    reset_all();
    len = build_param_read(req, "motor.pole_pairs");
    CHECK(transact(MP_CMD_PARAM_READ, 10U, req, len, &resp, &rlen) == MP_STATUS_OK);
    CHECK(rlen == 6U);
    CHECK(resp[1] == MP_TYPE_U32); /* U8 元数据在线格式上报为 U32 */
    CHECK(u32_at(resp, 2) == 10U);
}

static void test_param_write(void) {
    const uint8_t *resp;
    uint8_t req[64];
    uint16_t len;
    uint16_t rlen;

    reset_all();
    g_mock.foc_state = APP_FOC_STATE_OFF;

    len = build_param_write(req, "motor.pole_pairs", MP_TYPE_U32, 12U);
    CHECK(transact(MP_CMD_PARAM_WRITE, 11U, req, len, &resp, &rlen) == MP_STATUS_OK);
    CHECK(app_motor_params_mutable()->pole_pairs == 12U);

    /* 越界 */
    len = build_param_write(req, "motor.pole_pairs", MP_TYPE_U32, 0U);
    CHECK(transact(MP_CMD_PARAM_WRITE, 12U, req, len, &resp, &rlen) == MP_STATUS_RANGE_ERR);
    CHECK(app_motor_params_mutable()->pole_pairs == 12U);

    /* 类型不符 */
    len = build_param_write(req, "motor.pole_pairs", MP_TYPE_F32, 12U);
    CHECK(transact(MP_CMD_PARAM_WRITE, 13U, req, len, &resp, &rlen) == MP_STATUS_BAD_PARAM);

    /* 未知名 */
    len = build_param_write(req, "no.such.param", MP_TYPE_U32, 1U);
    CHECK(transact(MP_CMD_PARAM_WRITE, 14U, req, len, &resp, &rlen) == MP_STATUS_BAD_PARAM);
}

static void test_param_write_foc_off(void) {
    const uint8_t *resp;
    uint8_t req[64];
    uint16_t len;
    uint16_t rlen;

    reset_all();
    g_mock.foc_state = APP_FOC_STATE_RUN;
    len = build_param_write(req, "hardware.inverter.deadtime_comp_ns", MP_TYPE_U32, 150U);
    CHECK(transact(MP_CMD_PARAM_WRITE, 15U, req, len, &resp, &rlen) == MP_STATUS_LOCKED);
    CHECK(app_hardware_params_mutable()->inverter.deadtime_comp_ns == 0U);

    g_mock.foc_state = APP_FOC_STATE_OFF;
    CHECK(transact(MP_CMD_PARAM_WRITE, 16U, req, len, &resp, &rlen) == MP_STATUS_OK);
    CHECK(app_hardware_params_mutable()->inverter.deadtime_comp_ns == 150U);
}

/** 写编码器零点/方向会清「已标定」标志。 */
static void test_param_write_encoder_flag(void) {
    const uint8_t *resp;
    uint8_t req[64];
    uint16_t len;
    uint16_t rlen;
    union {
        float f;
        uint32_t u;
    } bits;

    reset_all();
    g_mock.foc_state = APP_FOC_STATE_OFF;
    app_motor_params_mutable()->encoder.electrical_offset_valid = 1U;

    bits.f = 1.0f;
    len = build_param_write(req, "motor.encoder.direction", MP_TYPE_F32, bits.u);
    CHECK(transact(MP_CMD_PARAM_WRITE, 17U, req, len, &resp, &rlen) == MP_STATUS_OK);
    CHECK(app_motor_params_mutable()->encoder.electrical_offset_valid == 0U);
}

static void test_param_commit(void) {
    const uint8_t *resp;
    uint16_t rlen;

    reset_all();
    g_mock.foc_state = APP_FOC_STATE_RUN;
    CHECK(transact(MP_CMD_PARAM_COMMIT, 18U, NULL, 0U, &resp, &rlen) == MP_STATUS_LOCKED);

    g_mock.foc_state = APP_FOC_STATE_OFF;
    g_mock.stop_pending = true;
    CHECK(transact(MP_CMD_PARAM_COMMIT, 19U, NULL, 0U, &resp, &rlen) == MP_STATUS_LOCKED);

    g_mock.stop_pending = false;
    CHECK(transact(MP_CMD_PARAM_COMMIT, 20U, NULL, 0U, &resp, &rlen) == MP_STATUS_OK);

    g_mock_flash_fail_store = true;
    CHECK(transact(MP_CMD_PARAM_COMMIT, 21U, NULL, 0U, &resp, &rlen) == MP_STATUS_BUSY);
    g_mock_flash_fail_store = false;
}

static void test_param_reset(void) {
    const uint8_t *resp;
    static const char *const bad_names[] = {"motor.pole_pairs", "no.such.param"};
    static const char *const one_name[] = {"motor.pole_pairs"};
    uint8_t req[128];
    uint16_t len;
    uint16_t rlen;

    reset_all();
    g_mock.foc_state = APP_FOC_STATE_RUN;
    len = build_reset(req, one_name, 1U);
    CHECK(transact(MP_CMD_PARAM_RESET, 22U, req, len, &resp, &rlen) == MP_STATUS_LOCKED);

    g_mock.foc_state = APP_FOC_STATE_OFF;
    app_motor_params_mutable()->pole_pairs = 42U;

    /* 名单含未知名：整体拒绝，不做部分恢复 */
    len = build_reset(req, bad_names, 2U);
    CHECK(transact(MP_CMD_PARAM_RESET, 23U, req, len, &resp, &rlen) == MP_STATUS_BAD_PARAM);
    CHECK(app_motor_params_mutable()->pole_pairs == 42U);

    len = build_reset(req, one_name, 1U);
    CHECK(transact(MP_CMD_PARAM_RESET, 24U, req, len, &resp, &rlen) == MP_STATUS_OK);
    CHECK(app_motor_params_mutable()->pole_pairs == 10U);

    /* 全部恢复：count=0 */
    app_motor_params_mutable()->pole_pairs = 42U;
    app_software_params_mutable()->fault.actions_enable = 0U;
    req[0] = 0U;
    CHECK(transact(MP_CMD_PARAM_RESET, 25U, req, 1U, &resp, &rlen) == MP_STATUS_OK);
    CHECK(app_motor_params_mutable()->pole_pairs == 10U);
    CHECK(app_software_params_mutable()->fault.actions_enable == 1U);
    CHECK(app_motor_params_mutable()->encoder.electrical_offset_valid == 0U);
}

/* ============================================================================
 * 故障 / 控制
 * ============================================================================ */

static void test_fault(void) {
    const uint8_t *resp;
    uint16_t rlen;

    reset_all();
    g_mock.fault_codes = 0x00000A00U;
    g_mock.fault_latched = 0x00000200U;
    CHECK(transact(MP_CMD_FAULT_GET, 26U, NULL, 0U, &resp, &rlen) == MP_STATUS_OK);
    CHECK(rlen == 9U);
    CHECK(u32_at(resp, 1) == 0x00000A00U);
    CHECK(u32_at(resp, 5) == 0x00000200U);

    CHECK(transact(MP_CMD_FAULT_CLEAR, 27U, NULL, 0U, &resp, &rlen) == MP_STATUS_OK);
    CHECK(g_mock.fault_codes == 0U);
    CHECK(g_mock.fault_latched == 0U);

    g_mock.fault_clear_rc = -1;
    CHECK(transact(MP_CMD_FAULT_CLEAR, 28U, NULL, 0U, &resp, &rlen) == MP_STATUS_LOCKED);
}

static void test_ctrl_start_stop(void) {
    const uint8_t *resp;
    uint8_t req[8];
    uint16_t rlen;

    reset_all();
    g_mock.foc_enable_rc = 0;
    CHECK(transact(MP_CMD_CTRL_CMD, 29U, req, ctrl_req(MP_CTRL_START, 0.0f, req), &resp,
                   &rlen) == MP_STATUS_OK);
    CHECK(g_mock.foc_state == APP_FOC_STATE_READY);

    g_mock.foc_enable_rc = -2;
    CHECK(transact(MP_CMD_CTRL_CMD, 30U, req, ctrl_req(MP_CTRL_START, 0.0f, req), &resp,
                   &rlen) == MP_STATUS_LOCKED);

    g_mock.stop_rc = 0;
    CHECK(transact(MP_CMD_CTRL_CMD, 31U, req, ctrl_req(MP_CTRL_STOP, 0.0f, req), &resp,
                   &rlen) == MP_STATUS_OK);

    g_mock.stop_rc = -1;
    CHECK(transact(MP_CMD_CTRL_CMD, 32U, req, ctrl_req(MP_CTRL_STOP, 0.0f, req), &resp,
                   &rlen) == MP_STATUS_LOCKED);
}

/** SET_MODE 的线上编码是 f32 位型低字节；数值 2.0 必须被拒绝。 */
static void test_ctrl_mode(void) {
    const uint8_t *resp;
    uint8_t req[8];
    uint16_t rlen;
    union {
        float f;
        uint32_t u;
    } bits;

    reset_all();
    g_mock.motion_snap.mode = APP_MOTION_MODE_POSITION;

    bits.u = 2U; /* mode 2 = 速度，f32 位型低字节 */
    CHECK(transact(MP_CMD_CTRL_CMD, 33U, req, ctrl_req(MP_CTRL_SET_MODE, bits.f, req), &resp,
                   &rlen) == MP_STATUS_OK);
    CHECK(g_mock.release_count == 1); /* POSITION -> SPEED 跨模式 */

    bits.u = 1U; /* mode 1 = 力矩；当前仍是 POSITION */
    CHECK(transact(MP_CMD_CTRL_CMD, 34U, req, ctrl_req(MP_CTRL_SET_MODE, bits.f, req), &resp,
                   &rlen) == MP_STATUS_OK);
    CHECK(g_mock.release_count == 2);

    /* 数值 2.0 不是合法编码 */
    CHECK(transact(MP_CMD_CTRL_CMD, 35U, req, ctrl_req(MP_CTRL_SET_MODE, 2.0f, req), &resp,
                   &rlen) == MP_STATUS_BAD_PARAM);

    bits.u = 9U;
    CHECK(transact(MP_CMD_CTRL_CMD, 36U, req, ctrl_req(MP_CTRL_SET_MODE, bits.f, req), &resp,
                   &rlen) == MP_STATUS_BAD_PARAM);
}

static void test_ctrl_setpoints(void) {
    const uint8_t *resp;
    uint8_t req[8];
    uint16_t rlen;
    union {
        float f;
        uint32_t u;
    } bits;

    reset_all();
    g_mock.motion_rc = 0;

    CHECK(transact(MP_CMD_CTRL_CMD, 37U, req, ctrl_req(MP_CTRL_SET_TORQUE, 0.5f, req), &resp,
                   &rlen) == MP_STATUS_OK);
    CHECK(g_mock.motion_call == 1);
    CHECK_NEAR(g_mock.motion_arg, 0.5f, 1e-6f);

    CHECK(transact(MP_CMD_CTRL_CMD, 38U, req, ctrl_req(MP_CTRL_SET_SPEED, 5.0f, req), &resp,
                   &rlen) == MP_STATUS_OK);
    CHECK(g_mock.motion_call == 2);
    CHECK_NEAR(g_mock.motion_arg, 5.0f * 0.10471975512f, 1e-6f);

    CHECK(transact(MP_CMD_CTRL_CMD, 39U, req, ctrl_req(MP_CTRL_SET_ANGLE, 90.0f, req), &resp,
                   &rlen) == MP_STATUS_OK);
    CHECK(g_mock.motion_call == 3);
    CHECK_NEAR(g_mock.motion_arg, 1.57079632679f, 1e-6f);

    /* 越界与非有限值 */
    CHECK(transact(MP_CMD_CTRL_CMD, 40U, req, ctrl_req(MP_CTRL_SET_SPEED, 30000.0f, req), &resp,
                   &rlen) == MP_STATUS_RANGE_ERR);
    bits.u = 0x7FC00000U; /* NaN */
    CHECK(transact(MP_CMD_CTRL_CMD, 41U, req, ctrl_req(MP_CTRL_SET_TORQUE, bits.f, req), &resp,
                   &rlen) == MP_STATUS_RANGE_ERR);

    /* motion 返回码映射 */
    g_mock.motion_rc = APP_MOTION_CMD_BAD_PARAM;
    CHECK(transact(MP_CMD_CTRL_CMD, 42U, req, ctrl_req(MP_CTRL_SET_TORQUE, 0.1f, req), &resp,
                   &rlen) == MP_STATUS_BAD_PARAM);
    g_mock.motion_rc = APP_MOTION_CMD_TARGET_LIMIT;
    CHECK(transact(MP_CMD_CTRL_CMD, 43U, req, ctrl_req(MP_CTRL_SET_TORQUE, 0.1f, req), &resp,
                   &rlen) == MP_STATUS_RANGE_ERR);
    g_mock.motion_rc = APP_MOTION_CMD_FOC_NOT_READY;
    CHECK(transact(MP_CMD_CTRL_CMD, 44U, req, ctrl_req(MP_CTRL_SET_TORQUE, 0.1f, req), &resp,
                   &rlen) == MP_STATUS_LOCKED);
}

static void test_ctrl_unsupported(void) {
    const uint8_t *resp;
    uint8_t req[8];
    uint16_t rlen;
    static const uint8_t unsupported[] = {
        MP_CTRL_SET_FREQ, MP_CTRL_SET_MOD, MP_CTRL_SET_OUTPUT_ZERO_NOW,
        MP_CTRL_SET_OUTPUT_ZERO_ABS,
    };

    reset_all();
    for (uint8_t i = 0U; i < (uint8_t) sizeof(unsupported); i++) {
        CHECK(transact(MP_CMD_CTRL_CMD, (uint8_t) (45U + i), req,
                       ctrl_req(unsupported[i], 0.0f, req), &resp, &rlen)
              == MP_STATUS_UNSUPPORTED);
    }
}

/* ============================================================================
 * 遥测流
 * ============================================================================ */

/** 解析最近一帧遥测：校验帧头/CRC/长度，返回 payload 指针与样本数。 */
static const uint8_t *last_stream(uint16_t *count, uint32_t *index) {
    static uint8_t copy[600];
    uint16_t len;
    uint16_t crc = 0xFFFFU;

    CHECK(g_mock.tx_count > 0);
    CHECK(g_mock.tx_len >= 9U);
    CHECK(g_mock.tx[0] == SOF0);
    CHECK(g_mock.tx[1] == SOF1);
    CHECK(g_mock.tx[2] == VER);
    CHECK(g_mock.tx[3] == MP_CMD_STREAM_DATA);

    len = u16_at(g_mock.tx, 5);
    CHECK(g_mock.tx_len == (uint16_t) (9U + len));
    for (uint16_t i = 2U; i < (uint16_t) (7U + len); i++) {
        crc = crc16_update(crc, g_mock.tx[i]);
    }
    CHECK(crc == u16_at(g_mock.tx, (uint16_t) (7U + len)));

    memcpy(copy, &g_mock.tx[7], len);
    *index = u32_at(copy, 0);
    *count = u16_at(copy, 4);
    CHECK(copy[6] == 72U);
    CHECK(len == (uint16_t) (8U + (*count) * 72U));
    return copy;
}

static void test_stream_basic(void) {
    const uint8_t *resp;
    const uint8_t *p;
    uint8_t req[8];
    uint16_t rlen;
    uint16_t count;
    uint32_t index;

    reset_all();
    req[0] = 0xE8U; /* 1000 LE */
    req[1] = 0x03U;
    req[2] = 0x07U; /* batch 7 */
    req[3] = 0x00U;
    CHECK(transact(MP_CMD_STREAM_SUB, 60U, req, 4U, &resp, &rlen) == MP_STATUS_OK);
    CHECK(g_mock.tx_count == 1); /* SUB 应答 */
    g_mock.tx_count = 0;

    /* SUB 的服务调用已采 1 个样本；再补 6 次满批 */
    for (int i = 0; (i < 10) && (g_mock.tx_count == 0); i++) {
        g_mock.cycles += 480000U;
        app_usb_proto_service();
    }
    CHECK(g_mock.tx_count == 1);

    p = last_stream(&count, &index);
    CHECK(index == 0U);
    CHECK(count == 7U);
    CHECK_NEAR(f32_at(p, 8U + 4U), 1.0f, 1e-6f);   /* i_u */
    CHECK_NEAR(f32_at(p, 8U + 16U), 24.0f, 1e-6f); /* v_bus */
    CHECK_NEAR(f32_at(p, 8U + 20U), 25.0f, 1e-6f); /* ntc0 */
    CHECK_NEAR(f32_at(p, 8U + 44U), 0.25f, 1e-6f); /* i_d */
    CHECK_NEAR(f32_at(p, 8U + 56U), 2.0f, 1e-6f);  /* torque */
    CHECK(u16_at(p, 8U + 68U) == 1234U);            /* rotor_raw */
    CHECK(u16_at(p, 8U + 70U) == 5678U);            /* output_raw */

    /* 下一批：sample_index 严格 +7 */
    g_mock.tx_count = 0;
    for (int i = 0; i < 7; i++) {
        g_mock.cycles += 480000U;
        app_usb_proto_service();
    }
    CHECK(g_mock.tx_count == 1);
    p = last_stream(&count, &index);
    CHECK(index == 7U);
    CHECK(count == 7U);

    /* 退订后不再发送 */
    g_mock.tx_count = 0;
    CHECK(transact(MP_CMD_STREAM_UNSUB, 61U, NULL, 0U, &resp, &rlen) == MP_STATUS_OK);
    g_mock.tx_count = 0;
    for (int i = 0; i < 10; i++) {
        g_mock.cycles += 480000U;
        app_usb_proto_service();
    }
    CHECK(g_mock.tx_count == 0);
}

static void test_stream_ranges_and_drop(void) {
    const uint8_t *resp;
    uint8_t req[8];
    uint16_t rlen;

    reset_all();
    req[0] = 0xD0U; /* 2000 LE */
    req[1] = 0x07U;
    req[2] = 0x07U;
    req[3] = 0x00U;
    CHECK(transact(MP_CMD_STREAM_SUB, 62U, req, 4U, &resp, &rlen) == MP_STATUS_RANGE_ERR);

    req[0] = 0xE8U;
    req[1] = 0x03U;
    req[2] = 0x08U; /* batch 8 */
    req[3] = 0x00U;
    CHECK(transact(MP_CMD_STREAM_SUB, 63U, req, 4U, &resp, &rlen) == MP_STATUS_RANGE_ERR);

    /* 未就绪：整批丢弃，不发送 */
    req[2] = 0x07U;
    CHECK(transact(MP_CMD_STREAM_SUB, 64U, req, 4U, &resp, &rlen) == MP_STATUS_OK);
    g_mock.tx_count = 0;
    g_mock.ready = false;
    for (int i = 0; i < 10; i++) {
        g_mock.cycles += 480000U;
        app_usb_proto_service();
    }
    CHECK(g_mock.tx_count == 0);

    /* 恢复就绪：重新累积满批后发送 */
    g_mock.ready = true;
    for (int i = 0; (i < 10) && (g_mock.tx_count == 0); i++) {
        g_mock.cycles += 480000U;
        app_usb_proto_service();
    }
    CHECK(g_mock.tx_count == 1);
}

/* ============================================================================
 * 应答重试槽
 * ============================================================================ */

/** 端点忙时应答进 pending，下一拍重发。 */
static void test_tx_pending(void) {
    uint8_t frame[64];
    uint16_t flen;

    reset_all();
    flen = build_req(MP_CMD_PROTO_INFO, 70U, NULL, 0U, frame);

    g_mock.write_rc = -1;
    feed(frame, flen);
    run_service();
    CHECK(g_mock.tx_count == 0);

    g_mock.write_rc = 0;
    app_usb_proto_service(); /* 无 RX，仅重发 pending */
    CHECK(g_mock.tx_count == 1);
    (void) last_payload(MP_CMD_PROTO_INFO, 70U, &flen);
}

/* ============================================================================
 * 入口
 * ============================================================================ */

void test_usb_proto(void) {
    test_info_golden();
    test_bad_crc_resync();
    test_oversize_len();
    test_garbage_resync();
    test_partial_frame();
    test_two_requests_one_chunk();
    test_param_read();
    test_param_write();
    test_param_write_foc_off();
    test_param_write_encoder_flag();
    test_param_commit();
    test_param_reset();
    test_fault();
    test_ctrl_start_stop();
    test_ctrl_mode();
    test_ctrl_setpoints();
    test_ctrl_unsupported();
    test_stream_basic();
    test_stream_ranges_and_drop();
    test_tx_pending();
}
