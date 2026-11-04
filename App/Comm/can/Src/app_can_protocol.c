/**
 * @file    app_can_protocol.c
 * @brief   DM 兼容 CAN 协议纯编解码实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_can_protocol.h"

#include <stddef.h>

static const app_can_dm_scale_t s_dm_default_scale = {
    .p_max_rad = APP_CAN_DM_P_MAX_RAD,
    .v_max_rad_s = APP_CAN_DM_V_MAX_RAD_S,
    .t_ff_max_nm = APP_CAN_DM_T_MAX_NM,
    .t_fb_max_nm = APP_CAN_DM_T_FB_MAX_NM,
    .kp_max = APP_CAN_DM_KP_MAX,
    .kd_max = APP_CAN_DM_KD_MAX,
};

/* ── 基础工具 ─────────────────────────────────────────────────────── */

/** @brief 位模式有限性判定（-ffast-math 下 isnan/isfinite 不可用） */
static bool dm_finite(float x) {
    union {
        float f;
        uint32_t u;
    } pun = {.f = x};

    return (pun.u & 0x7F800000U) != 0x7F800000U;
}

/** @brief 小端读取 u16 */
static uint16_t get_u16(const uint8_t* in) {
    return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

/** @brief 小端读取 f32（IEEE754） */
static float get_f32(const uint8_t* in) {
    uint32_t bits = (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16)
                    | ((uint32_t)in[3] << 24);
    float value;

    union {
        float f;
        uint32_t u;
    } pun = {.u = bits};

    value = pun.f;
    return value;
}

const app_can_dm_scale_t* app_can_dm_default_scale(void) { return &s_dm_default_scale; }

bool app_can_dm_parse_id(uint32_t id, uint8_t* type, uint8_t* node) {
    if ((id > 0x7FFU) || ((id >> 8) != 0x1U)) {
        return false;
    }
    uint8_t t = (uint8_t)((id >> 4) & 0x0FU);

    if ((t < APP_CAN_DM_TYPE_MIT) || (t > APP_CAN_DM_TYPE_FEEDBACK)) {
        return false; /* 类型 0 与 9..15 保留 */
    }
    if (type != NULL) {
        *type = t;
    }
    if (node != NULL) {
        *node = (uint8_t)(id & 0x0FU);
    }
    return true;
}

uint32_t app_can_dm_make_id(uint8_t type, uint8_t node) {
    return APP_CAN_DM_ID_BASE | (((uint32_t)type & 0x0FU) << 4) | ((uint32_t)node & 0x0FU);
}

uint32_t app_can_dm_feedback_id(uint8_t node) {
    return APP_CAN_DM_FEEDBACK_BASE | ((uint32_t)node & 0x0FU);
}

uint32_t app_can_dm_float_to_uint(float x, float min, float max, uint32_t bits) {
    uint32_t max_raw;

    if ((bits == 0U) || (bits > 16U) || (max <= min) || !dm_finite(x)) {
        return 0U;
    }
    max_raw = (1UL << bits) - 1UL;
    if (x <= min) {
        return 0U;
    }
    if (x >= max) {
        return max_raw;
    }
    return (uint32_t)((x - min) * (float)max_raw / (max - min));
}

float app_can_dm_uint_to_float(uint32_t x, float min, float max, uint32_t bits) {
    uint32_t max_raw;

    if ((bits == 0U) || (bits > 16U) || (max <= min)) {
        return min;
    }
    max_raw = (1UL << bits) - 1UL;
    return (float)x * (max - min) / (float)max_raw + min;
}

int app_can_dm_decode(uint32_t id, const uint8_t* data, uint8_t len, uint8_t node,
                      const app_can_dm_scale_t* scale, app_can_dm_frame_t* out) {
    uint8_t type = 0U;
    uint8_t frame_node = 0U;
    const app_can_dm_scale_t* sc = (scale != NULL) ? scale : &s_dm_default_scale;

    if ((data == NULL) || (out == NULL)) {
        return -1;
    }
    out->kind = APP_CAN_DM_KIND_NONE;
    out->p_des_rad = 0.0f;
    out->v_des_rad_s = 0.0f;
    out->kp = 0.0f;
    out->kd = 0.0f;
    out->t_ff_nm = 0.0f;
    out->vel_limit_rad_s = 0.0f;
    out->iq_ratio = 0.0f;

    if (!app_can_dm_parse_id(id, &type, &frame_node) || (frame_node != node)) {
        return 1; /* 非本机/未知类型 */
    }

    switch (type) {
    case APP_CAN_DM_TYPE_MIT: {
        if (len < 8U) {
            return -1;
        }
        uint32_t p_raw = ((uint32_t)data[0] << 8) | (uint32_t)data[1];
        uint32_t v_raw = ((uint32_t)data[2] << 4) | ((uint32_t)data[3] >> 4);
        uint32_t kp_raw = (((uint32_t)data[3] & 0x0FU) << 8) | (uint32_t)data[4];
        uint32_t kd_raw = ((uint32_t)data[5] << 4) | ((uint32_t)data[6] >> 4);
        uint32_t t_raw = (((uint32_t)data[6] & 0x0FU) << 8) | (uint32_t)data[7];

        out->kind = APP_CAN_DM_KIND_MIT;
        out->p_des_rad = app_can_dm_uint_to_float(p_raw, -sc->p_max_rad, sc->p_max_rad, 16U);
        out->v_des_rad_s = app_can_dm_uint_to_float(v_raw, -sc->v_max_rad_s, sc->v_max_rad_s, 12U);
        out->kp = app_can_dm_uint_to_float(kp_raw, 0.0f, sc->kp_max, 12U);
        out->kd = app_can_dm_uint_to_float(kd_raw, 0.0f, sc->kd_max, 12U);
        out->t_ff_nm = app_can_dm_uint_to_float(t_raw, -sc->t_ff_max_nm, sc->t_ff_max_nm, 12U);
        return 0;
    }
    case APP_CAN_DM_TYPE_POS_VEL: {
        if (len < 8U) {
            return -1;
        }
        float p = get_f32(&data[0]);
        float v = get_f32(&data[4]);

        if (!dm_finite(p) || !dm_finite(v)) {
            return -1;
        }
        out->kind = APP_CAN_DM_KIND_POS_VEL;
        out->p_des_rad = p;
        out->v_des_rad_s = v;
        return 0;
    }
    case APP_CAN_DM_TYPE_VEL: {
        if (len < 4U) {
            return -1;
        }
        float v = get_f32(&data[0]);

        if (!dm_finite(v)) {
            return -1;
        }
        out->kind = APP_CAN_DM_KIND_VEL;
        out->v_des_rad_s = v;
        return 0;
    }
    case APP_CAN_DM_TYPE_FORCE_POS: {
        if (len < 8U) {
            return -1;
        }
        float p = get_f32(&data[0]);
        uint16_t v_raw = get_u16(&data[4]);
        uint16_t iq_raw = get_u16(&data[6]);

        if (!dm_finite(p)) {
            return -1;
        }
        out->kind = APP_CAN_DM_KIND_FORCE_POS;
        out->p_des_rad = p;
        out->vel_limit_rad_s = (float)v_raw / 100.0f;
        out->iq_ratio = (iq_raw > 10000U) ? 1.0f : ((float)iq_raw / 10000.0f);
        return 0;
    }
    case APP_CAN_DM_TYPE_ENABLE:
    case APP_CAN_DM_TYPE_DISABLE:
    case APP_CAN_DM_TYPE_CLEAR: {
        uint8_t magic = (type == APP_CAN_DM_TYPE_ENABLE)  ? APP_CAN_DM_MAGIC_ENABLE
                        : (type == APP_CAN_DM_TYPE_DISABLE) ? APP_CAN_DM_MAGIC_DISABLE
                                                            : APP_CAN_DM_MAGIC_CLEAR;

        if ((len < 1U) || (data[0] != magic)) {
            return -1;
        }
        out->kind = (type == APP_CAN_DM_TYPE_ENABLE)
                        ? APP_CAN_DM_KIND_ENABLE
                        : (type == APP_CAN_DM_TYPE_DISABLE) ? APP_CAN_DM_KIND_DISABLE
                                                            : APP_CAN_DM_KIND_CLEAR;
        return 0;
    }
    default:
        return 1; /* 类型 9..15 保留 */
    }
}

uint8_t app_can_dm_encode_feedback(uint8_t* out, uint8_t node, uint8_t err, float pos_rad,
                                   float vel_rad_s, float torque_nm, uint8_t t_mos_c,
                                   uint8_t t_rotor_c, const app_can_dm_scale_t* scale) {
    const app_can_dm_scale_t* sc = (scale != NULL) ? scale : &s_dm_default_scale;
    uint32_t pos_raw;
    uint32_t vel_raw;
    uint32_t t_raw;

    if (out == NULL) {
        return 0U;
    }
    pos_raw = app_can_dm_float_to_uint(pos_rad, -sc->p_max_rad, sc->p_max_rad, 16U);
    vel_raw = app_can_dm_float_to_uint(vel_rad_s, -sc->v_max_rad_s, sc->v_max_rad_s, 12U);
    t_raw = app_can_dm_float_to_uint(torque_nm, -sc->t_fb_max_nm, sc->t_fb_max_nm, 12U);

    out[0] = (uint8_t)(((err & 0x0FU) << 4) | (node & 0x0FU));
    out[1] = (uint8_t)(pos_raw >> 8);
    out[2] = (uint8_t)(pos_raw & 0xFFU);
    out[3] = (uint8_t)(vel_raw >> 4);
    out[4] = (uint8_t)(((vel_raw & 0x0FU) << 4) | (t_raw >> 8));
    out[5] = (uint8_t)(t_raw & 0xFFU);
    out[6] = t_mos_c;
    out[7] = t_rotor_c;
    return 8U;
}
