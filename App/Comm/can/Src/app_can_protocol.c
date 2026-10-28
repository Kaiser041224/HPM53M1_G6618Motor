/**
 * @file    app_can_protocol.c
 * @brief   CAN 报文编解码实现（纯函数）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_can_protocol.h"

#include <stddef.h>
#include <string.h>

/* ── 位域与定标辅助 ─────────────────────────────────────────────── */

/** @brief 写入位域（小端位序，bit0 = data[0] 的最低有效位） */
static void put_bits(uint8_t* out, uint32_t pos, uint32_t nbits, uint32_t value) {
    for (uint32_t i = 0U; i < nbits; i++) {
        uint32_t p = pos + i;

        if (((value >> i) & 1U) != 0U) {
            out[p >> 3] |= (uint8_t)(1U << (p & 7U));
        } else {
            out[p >> 3] &= (uint8_t)~(1U << (p & 7U));
        }
    }
}

/** @brief 读取位域 */
static uint32_t get_bits(const uint8_t* in, uint32_t pos, uint32_t nbits) {
    uint32_t value = 0U;

    for (uint32_t i = 0U; i < nbits; i++) {
        uint32_t p = pos + i;

        if ((in[p >> 3] & (uint8_t)(1U << (p & 7U))) != 0U) {
            value |= (1U << i);
        }
    }
    return value;
}

/** @brief 符号扩展 */
static int32_t sign_extend(uint32_t value, uint32_t nbits) {
    uint32_t sign = 1U << (nbits - 1U);

    return (int32_t)((value ^ sign) - sign);
}

/** @brief 浮点 → 定标整数（四舍五入 + 饱和） */
static int32_t quantize(float value, float scale, int32_t lo, int32_t hi) {
    float scaled = value * scale;
    int32_t out;

    if (scaled >= (float)hi) {
        return hi;
    }
    if (scaled <= (float)lo) {
        return lo;
    }
    out = (int32_t)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
    if (out < lo) {
        out = lo;
    } else if (out > hi) {
        out = hi;
    }
    return out;
}

/** @brief 小端读取 u16 */
static uint16_t get_u16(const uint8_t* in) {
    return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

/** @brief 小端写入 u32 */
static void put_u32(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8) & 0xFFU);
    out[2] = (uint8_t)((value >> 16) & 0xFFU);
    out[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/** @brief 小端读取 u32 */
static uint32_t get_u32(const uint8_t* in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16)
           | ((uint32_t)in[3] << 24);
}

/** @brief 小端写入 f32（IEEE754） */
static void put_f32(uint8_t* out, float value) {
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    put_u32(out, bits);
}

/** @brief 小端读取 f32 */
static float get_f32(const uint8_t* in) {
    uint32_t bits = get_u32(in);
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* ── ID ─────────────────────────────────────────────────────────── */

uint32_t app_can_make_id(uint8_t node_id, uint8_t cmd) {
    return ((uint32_t)node_id << 5) | ((uint32_t)cmd & APP_CAN_CMD_MASK);
}

bool app_can_parse_id(uint32_t id, uint8_t* node_id, uint8_t* cmd) {
    if (id > 0x7FFU) {
        return false;
    }
    if (node_id != NULL) {
        *node_id = (uint8_t)(id >> 5);
    }
    if (cmd != NULL) {
        *cmd = (uint8_t)(id & APP_CAN_CMD_MASK);
    }
    return true;
}

/* ── 解码 ───────────────────────────────────────────────────────── */

int app_can_decode(uint32_t id, const uint8_t* data, uint8_t len, uint8_t node_id,
                   app_can_frame_t* out) {
    uint8_t frame_node = 0U;
    uint8_t cmd = 0U;

    if ((data == NULL) || (out == NULL) || !app_can_parse_id(id, &frame_node, &cmd)) {
        return -1;
    }
    out->kind = APP_CAN_FRAME_NONE;
    if (frame_node != node_id) {
        return 1; /* 非本节点：不是错误，交给上层忽略 */
    }

    switch (cmd) {
    case APP_CAN_CMD_ESTOP:
        out->kind = APP_CAN_FRAME_ESTOP;
        return 0;
    case APP_CAN_CMD_SET_AXIS_STATE:
        if (len < 1U) {
            return -1;
        }
        out->kind = APP_CAN_FRAME_AXIS_STATE;
        out->u.axis_state = data[0];
        return 0;
    case APP_CAN_CMD_SET_CONTROLLER_MODE:
        if (len < 2U) {
            return -1;
        }
        out->kind = APP_CAN_FRAME_CONTROLLER_MODE;
        out->u.mode.control_mode = data[0];
        out->u.mode.input_mode = data[1];
        return 0;
    case APP_CAN_CMD_SET_INPUT_POS:
        if (len < 8U) {
            return -1;
        }
        out->kind = APP_CAN_FRAME_INPUT_POS;
        out->u.pos.pos_deg = get_f32(&data[0]);
        out->u.pos.vel_ff_rpm = (float)(int16_t)get_u16(&data[4]) * 0.001f;
        out->u.pos.torque_ff_nm = (float)(int16_t)get_u16(&data[6]) * 0.001f;
        return 0;
    case APP_CAN_CMD_SET_INPUT_VEL:
        if (len < 6U) {
            return -1;
        }
        out->kind = APP_CAN_FRAME_INPUT_VEL;
        out->u.vel.vel_rpm = get_f32(&data[0]);
        out->u.vel.torque_ff_nm = (float)(int16_t)get_u16(&data[4]) * 0.001f;
        return 0;
    case APP_CAN_CMD_SET_INPUT_TORQUE:
        if (len < 4U) {
            return -1;
        }
        out->kind = APP_CAN_FRAME_INPUT_TORQUE;
        out->u.torque_nm = get_f32(&data[0]);
        return 0;
    case APP_CAN_CMD_SET_LIMITS:
        if (len < 8U) {
            return -1;
        }
        out->kind = APP_CAN_FRAME_LIMITS;
        out->u.limits.vel_limit_rpm = get_f32(&data[0]);
        out->u.limits.current_limit_a = get_f32(&data[4]);
        return 0;
    case APP_CAN_CMD_SET_INPUT_MIT:
        if (len < 8U) {
            return -1;
        }
        /* 单帧压缩：p(i16,0.1deg) v(i12,1rpm) kp(u12 归一化) kd(u12 归一化)
         * t_ff(i12,0.01Nm) = 64 bit。kp/kd 为 0..1 归一化值，
         * 由 service 层乘 software.control.mit.kp_max/kd_max。 */
        out->kind = APP_CAN_FRAME_MIT;
        out->u.mit.p_deg = (float)sign_extend(get_bits(data, 0U, 16U), 16U) * 0.1f;
        out->u.mit.v_rpm = (float)sign_extend(get_bits(data, 16U, 12U), 12U);
        out->u.mit.kp = (float)get_bits(data, 28U, 12U) * (1.0f / 4095.0f);
        out->u.mit.kd = (float)get_bits(data, 40U, 12U) * (1.0f / 4095.0f);
        out->u.mit.t_ff = (float)sign_extend(get_bits(data, 52U, 12U), 12U) * 0.01f;
        return 0;
    case APP_CAN_CMD_CLEAR_ERRORS:
        if (len < 4U) {
            return -1;
        }
        out->kind = APP_CAN_FRAME_CLEAR_ERRORS;
        out->u.identify = get_u32(&data[0]);
        return 0;
    default:
        out->kind = APP_CAN_FRAME_UNKNOWN;
        return 1;
    }
}

/* ── 编码 ───────────────────────────────────────────────────────── */

uint8_t app_can_encode_heartbeat(uint8_t* data, uint32_t errors, uint8_t axis_state,
                                 uint8_t procedure_result, uint8_t traj_done) {
    if (data == NULL) {
        return 0U;
    }
    put_u32(&data[0], errors);
    data[4] = axis_state;
    data[5] = procedure_result;
    data[6] = traj_done;
    data[7] = 0U;
    return 8U;
}

uint8_t app_can_encode_encoder(uint8_t* data, float pos_deg, float vel_rpm) {
    if (data == NULL) {
        return 0U;
    }
    put_f32(&data[0], pos_deg);
    put_f32(&data[4], vel_rpm);
    return 8U;
}

uint8_t app_can_encode_error(uint8_t* data, uint32_t active, uint32_t latched) {
    if (data == NULL) {
        return 0U;
    }
    put_u32(&data[0], active);
    put_u32(&data[4], latched);
    return 8U;
}

int app_can_decode_mit_a(const uint8_t* data, uint8_t len, app_can_mit_a_t* out) {
    if ((data == NULL) || (out == NULL) || (len < 8U)) {
        return -1;
    }
    out->p_deg = get_f32(&data[0]);
    out->v_rpm = (float)get_u16(&data[4]) * 0.1f;
    out->seq = data[6];
    return 0;
}

int app_can_decode_mit_b(const uint8_t* data, uint8_t len, app_can_mit_b_t* out) {
    if ((data == NULL) || (out == NULL) || (len < 8U)) {
        return -1;
    }
    out->kp = (float)get_u16(&data[0]) * 0.001f;
    out->kd = (float)get_u16(&data[2]) * 0.001f;
    out->t_ff = (float)(int16_t)get_u16(&data[4]) * 0.001f;
    out->seq = data[6];
    return 0;
}

uint8_t app_can_encode_mit(uint8_t* data, float p_deg, float v_rpm, float kp, float kd,
                           float t_ff, float kp_max, float kd_max) {
    if (data == NULL) {
        return 0U;
    }
    memset(data, 0, 8U);
    int32_t p_raw = quantize(p_deg, 10.0f, -32768, 32767);      /* 0.1 deg */
    int32_t v_raw = quantize(v_rpm, 1.0f, -2048, 2047);         /* 1 rpm */
    float kp_norm = (kp_max > 0.0f) ? (kp / kp_max) : 0.0f;
    float kd_norm = (kd_max > 0.0f) ? (kd / kd_max) : 0.0f;
    int32_t kp_raw = quantize(kp_norm, 4095.0f, 0, 4095);
    int32_t kd_raw = quantize(kd_norm, 4095.0f, 0, 4095);
    int32_t t_raw = quantize(t_ff, 100.0f, -2048, 2047);        /* 0.01 Nm */

    put_bits(data, 0U, 16U, (uint32_t)(p_raw & 0xFFFF));
    put_bits(data, 16U, 12U, (uint32_t)(v_raw & 0xFFF));
    put_bits(data, 28U, 12U, (uint32_t)(kp_raw & 0xFFF));
    put_bits(data, 40U, 12U, (uint32_t)(kd_raw & 0xFFF));
    put_bits(data, 52U, 12U, (uint32_t)(t_raw & 0xFFF));
    return 8U;
}