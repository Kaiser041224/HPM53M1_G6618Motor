/**
 * @file    app_usb_proto.c
 * @brief   USB bulk 二进制协议服务（motor_proto v1.3，P1a 只读子集）
 * @author  Kaiser
 *
 * 帧格式与命令语义以 Gui/protocol/motor_proto.h + protocol_spec.md 为准，
 * 本文件不重复定义常量，直接包含契约头。
 *
 * 上下文与预算：
 *   - 解码/分发/应答全部在 1kHz 慢任务（app_io_step）内执行；
 *   - 每拍最多从接收环取 PROTO_SERVICE_READ_MAX 字节，超出留到下一拍；
 *   - 应答写为任务上下文阻塞调用（超时 50ms），失败只丢帧不回滚。
 *
 * P1a 只读：PROTO_INFO / PARAM_META_* / PARAM_READ / STATUS_GET /
 *           FAULT_GET / FAULT_CLEAR；写类命令返回 UNSUPPORTED（caps 相应位为 0）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_usb_proto.h"

#include "app_adc.h"
#include "app_analog_signal.h"
#include "app_encoder.h"
#include "app_fault.h"
#include "app_foc.h"
#include "app_foc_current.h"
#include "app_param_meta.h"
#include "intf_usb_bulk.h"
#include "motor_proto.h"

#include <string.h>

/** 设备名（PROTO_INFO 返回；≤ 32 字节） */
#define PROTO_DEVICE_NAME     "HPM53M1"
#define PROTO_DEVICE_NAME_LEN (sizeof(PROTO_DEVICE_NAME) - 1U)

/** 每拍从接收环读取的上限 [byte] */
#define PROTO_SERVICE_READ_MAX (256U)

/** 参数名最大长度（含结尾 NUL 的缓冲为 64） */
#define PROTO_NAME_MAX (63U)

/* 能力位（protocol_spec §6.2）：bit0=PARAM_WRITE，bit1=CONTROL。
 * P1a 只读，两位均为 0；P1b 接入写/控制后在此置位。 */
#define PROTO_CAPS (0UL)

/** 应答写超时 [ms] */
#define PROTO_TX_TIMEOUT_MS (50U)

/* ============================================================================
 * 接收状态机
 * ============================================================================ */

typedef enum {
    PROTO_RX_SOF0 = 0,
    PROTO_RX_SOF1,
    PROTO_RX_VER,
    PROTO_RX_CMD,
    PROTO_RX_SEQ,
    PROTO_RX_LEN_LO,
    PROTO_RX_LEN_HI,
    PROTO_RX_PAYLOAD,
    PROTO_RX_CRC_LO,
    PROTO_RX_CRC_HI,
} proto_rx_state_t;

static proto_rx_state_t s_rx_state;
static uint8_t s_rx_cmd;
static uint8_t s_rx_seq;
static uint16_t s_rx_len;
static uint16_t s_rx_index;
static uint16_t s_rx_crc;
static uint16_t s_rx_crc_recv;
static uint8_t s_rx_payload[MP_MAX_PAYLOAD];

/** 参数名解码缓冲（静态，服务单线程） */
static char s_name[64];

/** 应答 payload（status + body）与最终帧（含 SOF/CRC） */
#define PROTO_RESP_PAYLOAD_MAX (1U + MP_MAX_PAYLOAD)
static uint8_t s_resp_payload[PROTO_RESP_PAYLOAD_MAX];
static uint8_t s_tx_frame[2U + 5U + PROTO_RESP_PAYLOAD_MAX + 2U];

/* ============================================================================
 * 字节序 / CRC 工具
 * ============================================================================ */

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t) (v & 0xFFU);
    p[1] = (uint8_t) ((v >> 8) & 0xFFU);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) (v & 0xFFU);
    p[1] = (uint8_t) ((v >> 8) & 0xFFU);
    p[2] = (uint8_t) ((v >> 16) & 0xFFU);
    p[3] = (uint8_t) ((v >> 24) & 0xFFU);
}

static void put_f32(uint8_t *p, float v) {
    union {
        float f;
        uint32_t u;
    } bits;

    bits.f = v;
    put_u32(p, bits.u);
}

/**
 * @brief CRC16-CCITT-FALSE 单字节更新
 * @param crc 当前值（首次传 0xFFFF）
 * @param byte 输入字节
 * @return 更新后的 CRC
 */
static uint16_t crc16_update(uint16_t crc, uint8_t byte) {
    crc ^= (uint16_t) ((uint16_t) byte << 8);
    for (uint8_t i = 0U; i < 8U; i++) {
        crc = ((crc & 0x8000U) != 0U) ? (uint16_t) ((crc << 1) ^ 0x1021U)
                                      : (uint16_t) (crc << 1);
    }
    return crc;
}

/* ============================================================================
 * 应答编码
 * ============================================================================ */

/**
 * @brief 仅状态字节的应答
 */
static uint16_t proto_status_only(uint8_t *out, uint8_t status) {
    out[0] = status;
    return 1U;
}

/**
 * @brief 元数据类型映射（固件元数据 → 线格式）
 *
 * 固件 U8/U16 统一上报为 U32（线格式只有 F32/U32/I32/BOOL）。
 * BOOL 暂不使用：固件元数据没有独立的布尔类型，避免按值域猜测。
 */
static uint8_t proto_type(uint8_t meta_type) {
    return (meta_type == PARAM_META_TYPE_F32) ? MP_TYPE_F32 : MP_TYPE_U32;
}

/**
 * @brief 编码 PARAM_META_GET 应答体
 */
static uint16_t proto_encode_meta(const param_meta_t *meta, uint16_t index, uint8_t *out) {
    const char *unit = (meta->unit != NULL) ? meta->unit : "";
    size_t name_len = strlen(meta->name);
    size_t unit_len = strlen(unit);
    uint16_t pos = 0U;

    /* status(1) + index(2) + type(1) + apply(1) + min/max(8) + 两个长度字节(2) */
    if ((name_len > 0xFFU) || (unit_len > 0xFFU)
        || ((15U + name_len + unit_len) > PROTO_RESP_PAYLOAD_MAX)) {
        return proto_status_only(out, MP_STATUS_BAD_PARAM);
    }

    out[pos++] = MP_STATUS_OK;
    put_u16(&out[pos], index);
    pos = (uint16_t) (pos + 2U);
    out[pos++] = proto_type(meta->type);
    out[pos++] = (meta->apply == PARAM_META_APPLY_LIVE) ? MP_APPLY_LIVE : MP_APPLY_REBOOT;
    put_f32(&out[pos], meta->min);
    pos = (uint16_t) (pos + 4U);
    put_f32(&out[pos], meta->max);
    pos = (uint16_t) (pos + 4U);
    out[pos++] = (uint8_t) name_len;
    memcpy(&out[pos], meta->name, name_len);
    pos = (uint16_t) (pos + name_len);
    out[pos++] = (uint8_t) unit_len;
    memcpy(&out[pos], unit, unit_len);
    pos = (uint16_t) (pos + unit_len);
    return pos;
}

/**
 * @brief 编码 PARAM_READ 应答体（type u8 + value u32 原始位型）
 */
static uint16_t proto_encode_value(const param_meta_t *meta, uint8_t *out) {
    const uint8_t *data = (const uint8_t *) app_param_meta_data(meta);
    uint32_t raw;

    if (data == NULL) {
        return proto_status_only(out, MP_STATUS_BAD_PARAM);
    }
    switch (meta->type) {
    case PARAM_META_TYPE_U8: raw = (uint32_t) *data; break;
    case PARAM_META_TYPE_U16: raw = (uint32_t) *(const uint16_t *) data; break;
    case PARAM_META_TYPE_U32: raw = *(const uint32_t *) data; break;
    default: {
        union {
            float f;
            uint32_t u;
        } bits;

        bits.f = *(const float *) data;
        raw = bits.u;
        break;
    }
    }

    out[0] = MP_STATUS_OK;
    out[1] = proto_type(meta->type);
    put_u32(&out[2], raw);
    return 6U;
}

/**
 * @brief 编码 STATUS_GET 应答体（布局见 motor_proto.h）
 *
 * freq_hz 仅在运行态取电流环快照的电角速度换算；停机态恒 0。
 *   —— OFF 时角度链仍以 PLL 跟踪转子，静止下 PLL 噪声可达 ±40Hz，
 *      直接上报会让上位机在停机时画出一条假频率曲线（台架实测）。
 * mod_pct 取电压圆利用率；温度取 NTC0，换算无效时上报 0（协议没有无效哨兵）。
 */
static uint16_t proto_encode_status(uint8_t *out) {
    app_foc_current_snapshot_t snap;
    app_analog_values_t analog;
    float deg_rotor = 0.0f;
    float deg_output = 0.0f;
    float ntc_c = 0.0f;
    app_foc_state_t state = app_foc_get_state();
    bool running = (state == APP_FOC_STATE_READY) || (state == APP_FOC_STATE_RUN)
                   || (state == APP_FOC_STATE_CALIB);
    uint16_t pos = 0U;

    app_foc_current_get_snapshot(&snap);
    (void) app_analog_signal_read_all(&analog);
    (void) app_analog_signal_ntc_temp_c(ADC_CH_NTC0, &ntc_c);
    (void) app_encoder_read_deg(APP_ENCODER_ROTOR, &deg_rotor);
    (void) app_encoder_read_deg(APP_ENCODER_OUTPUT, &deg_output);

    out[pos++] = MP_STATUS_OK;
    put_u32(&out[pos], app_fault_get_codes());
    pos = (uint16_t) (pos + 4U);
    out[pos++] = running ? 1U : 0U;
    out[pos++] = running ? (uint8_t) MP_MOTOR_RUN : (uint8_t) MP_MOTOR_STOP;
    put_f32(&out[pos], running ? (snap.omega_e_rad_s / 6.28318530718f) : 0.0f);
    pos = (uint16_t) (pos + 4U);
    put_f32(&out[pos], snap.voltage_utilization * 100.0f);
    pos = (uint16_t) (pos + 4U);
    put_f32(&out[pos], analog.i_u_a);
    pos = (uint16_t) (pos + 4U);
    put_f32(&out[pos], analog.i_v_a);
    pos = (uint16_t) (pos + 4U);
    put_f32(&out[pos], analog.i_w_a);
    pos = (uint16_t) (pos + 4U);
    put_f32(&out[pos], analog.v_bus_v);
    pos = (uint16_t) (pos + 4U);
    put_f32(&out[pos], ntc_c);
    pos = (uint16_t) (pos + 4U);
    put_f32(&out[pos], deg_rotor);
    pos = (uint16_t) (pos + 4U);
    put_f32(&out[pos], deg_output);
    pos = (uint16_t) (pos + 4U);
    return pos;
}

/* ============================================================================
 * 命令分发
 * ============================================================================ */

/**
 * @brief 执行一条请求，返回应答 payload 长度（payload[0] = status）
 */
static uint16_t proto_dispatch(uint8_t cmd, const uint8_t *payload, uint16_t len, uint8_t *out) {
    switch (cmd) {
    case MP_CMD_PROTO_INFO: {
        uint16_t pos = 0U;

        out[pos++] = MP_STATUS_OK;
        out[pos++] = MP_VERSION;
        out[pos++] = 0U; /* minor：与 gui-sim 保持一致，特性以 caps 为准 */
        put_u32(&out[pos], PROTO_CAPS);
        pos = (uint16_t) (pos + 4U);
        out[pos++] = (uint8_t) PROTO_DEVICE_NAME_LEN;
        memcpy(&out[pos], PROTO_DEVICE_NAME, PROTO_DEVICE_NAME_LEN);
        pos = (uint16_t) (pos + PROTO_DEVICE_NAME_LEN);
        return pos;
    }

    case MP_CMD_PARAM_META_COUNT: {
        out[0] = MP_STATUS_OK;
        put_u16(&out[1], (uint16_t) g_params_meta_count);
        return 3U;
    }

    case MP_CMD_PARAM_META_GET: {
        uint16_t index;

        if (len < 2U) {
            return proto_status_only(out, MP_STATUS_BAD_PARAM);
        }
        index = (uint16_t) (payload[0] | ((uint16_t) payload[1] << 8));
        if ((uint32_t) index >= g_params_meta_count) {
            return proto_status_only(out, MP_STATUS_RANGE_ERR);
        }
        return proto_encode_meta(&g_params_meta[index], index, out);
    }

    case MP_CMD_PARAM_READ: {
        uint8_t name_len;
        const param_meta_t *meta;

        if (len < 1U) {
            return proto_status_only(out, MP_STATUS_BAD_PARAM);
        }
        name_len = payload[0];
        if ((name_len == 0U) || (name_len > PROTO_NAME_MAX)
            || ((uint16_t) (1U + name_len) > len)) {
            return proto_status_only(out, MP_STATUS_BAD_PARAM);
        }
        memcpy(s_name, &payload[1], name_len);
        s_name[name_len] = '\0';

        meta = app_param_meta_find(s_name);
        if (meta == NULL) {
            return proto_status_only(out, MP_STATUS_BAD_PARAM);
        }
        return proto_encode_value(meta, out);
    }

    case MP_CMD_STATUS_GET:
        return proto_encode_status(out);

    case MP_CMD_FAULT_GET:
        out[0] = MP_STATUS_OK;
        put_u32(&out[1], app_fault_get_codes());
        put_u32(&out[5], app_fault_get_latched());
        return 9U;

    case MP_CMD_FAULT_CLEAR:
        return proto_status_only(out, (app_fault_clear() == 0) ? MP_STATUS_OK : MP_STATUS_LOCKED);

    /* P1a 未实现：caps 对应位为 0，按 spec §6.1 返回 UNSUPPORTED */
    case MP_CMD_PARAM_WRITE:
    case MP_CMD_PARAM_COMMIT:
    case MP_CMD_PARAM_RESET:
    case MP_CMD_CTRL_CMD:
    case MP_CMD_STREAM_SUB:
    case MP_CMD_STREAM_UNSUB:
        return proto_status_only(out, MP_STATUS_UNSUPPORTED);

    default:
        return proto_status_only(out, MP_STATUS_BAD_CMD);
    }
}

/**
 * @brief 组帧（SOF + 5 字节头 + payload + CRC）
 * @return 帧总长度
 */
static uint16_t proto_build_frame(uint8_t cmd, uint8_t seq, const uint8_t *payload,
                                  uint16_t len) {
    uint16_t crc = 0xFFFFU;
    uint16_t pos = 0U;

    s_tx_frame[pos++] = MP_SOF0;
    s_tx_frame[pos++] = MP_SOF1;

    s_tx_frame[pos] = MP_VERSION;
    crc = crc16_update(crc, s_tx_frame[pos++]);
    s_tx_frame[pos] = (uint8_t) (cmd | MP_RESPONSE_BIT);
    crc = crc16_update(crc, s_tx_frame[pos++]);
    s_tx_frame[pos] = seq;
    crc = crc16_update(crc, s_tx_frame[pos++]);
    s_tx_frame[pos] = (uint8_t) (len & 0xFFU);
    crc = crc16_update(crc, s_tx_frame[pos++]);
    s_tx_frame[pos] = (uint8_t) ((len >> 8) & 0xFFU);
    crc = crc16_update(crc, s_tx_frame[pos++]);

    for (uint16_t i = 0U; i < len; i++) {
        s_tx_frame[pos++] = payload[i];
        crc = crc16_update(crc, payload[i]);
    }

    s_tx_frame[pos++] = (uint8_t) (crc & 0xFFU);
    s_tx_frame[pos++] = (uint8_t) ((crc >> 8) & 0xFFU);
    return pos;
}

/**
 * @brief 处理一条完整请求并写回应答
 */
static void proto_handle_frame(void) {
    uint16_t payload_len;
    uint16_t frame_len;
    const intf_usb_bulk_t *bulk = intf_usb_bulk_get();

    payload_len = proto_dispatch(s_rx_cmd, s_rx_payload, s_rx_len, s_resp_payload);
    frame_len = proto_build_frame(s_rx_cmd, s_rx_seq, s_resp_payload, payload_len);

    if ((bulk != NULL) && bulk->write) {
        (void) bulk->write(s_tx_frame, frame_len, PROTO_TX_TIMEOUT_MS);
    }
}

/**
 * @brief 复位接收状态机
 */
static void proto_rx_reset(void) {
    s_rx_state = PROTO_RX_SOF0;
    s_rx_len = 0U;
    s_rx_index = 0U;
    s_rx_crc = 0xFFFFU;
    s_rx_crc_recv = 0U;
}

/**
 * @brief 喂入一个字节
 */
static void proto_rx_byte(uint8_t byte) {
    switch (s_rx_state) {
    case PROTO_RX_SOF0:
        if (byte == MP_SOF0) {
            s_rx_state = PROTO_RX_SOF1;
        }
        break;

    case PROTO_RX_SOF1:
        if (byte == MP_SOF1) {
            s_rx_state = PROTO_RX_VER;
        } else if (byte != MP_SOF0) {
            s_rx_state = PROTO_RX_SOF0;
        }
        break;

    case PROTO_RX_VER:
        if (byte != MP_VERSION) {
            proto_rx_reset();
            break;
        }
        s_rx_crc = crc16_update(0xFFFFU, byte);
        s_rx_state = PROTO_RX_CMD;
        break;

    case PROTO_RX_CMD:
        s_rx_cmd = byte;
        s_rx_crc = crc16_update(s_rx_crc, byte);
        s_rx_state = PROTO_RX_SEQ;
        break;

    case PROTO_RX_SEQ:
        s_rx_seq = byte;
        s_rx_crc = crc16_update(s_rx_crc, byte);
        s_rx_state = PROTO_RX_LEN_LO;
        break;

    case PROTO_RX_LEN_LO:
        s_rx_len = (uint16_t) byte;
        s_rx_crc = crc16_update(s_rx_crc, byte);
        s_rx_state = PROTO_RX_LEN_HI;
        break;

    case PROTO_RX_LEN_HI:
        s_rx_len = (uint16_t) (s_rx_len | ((uint16_t) byte << 8));
        s_rx_crc = crc16_update(s_rx_crc, byte);
        if (s_rx_len > MP_MAX_PAYLOAD) {
            proto_rx_reset();
        } else if (s_rx_len == 0U) {
            s_rx_state = PROTO_RX_CRC_LO;
        } else {
            s_rx_index = 0U;
            s_rx_state = PROTO_RX_PAYLOAD;
        }
        break;

    case PROTO_RX_PAYLOAD:
        s_rx_payload[s_rx_index++] = byte;
        s_rx_crc = crc16_update(s_rx_crc, byte);
        if (s_rx_index >= s_rx_len) {
            s_rx_state = PROTO_RX_CRC_LO;
        }
        break;

    case PROTO_RX_CRC_LO:
        s_rx_crc_recv = (uint16_t) byte;
        s_rx_state = PROTO_RX_CRC_HI;
        break;

    case PROTO_RX_CRC_HI:
        s_rx_crc_recv = (uint16_t) (s_rx_crc_recv | ((uint16_t) byte << 8));
        if (s_rx_crc_recv == s_rx_crc) {
            proto_handle_frame();
        }
        proto_rx_reset();
        break;

    default:
        proto_rx_reset();
        break;
    }
}

/* ============================================================================
 * 生命周期
 * ============================================================================ */

void app_usb_proto_init(void) {
    proto_rx_reset();
    s_rx_cmd = 0U;
    s_rx_seq = 0U;
    s_name[0] = '\0';
}

void app_usb_proto_service(void) {
    const intf_usb_bulk_t *bulk = intf_usb_bulk_get();
    uint8_t chunk[PROTO_SERVICE_READ_MAX];
    int count;

    if ((bulk == NULL) || (bulk->read == NULL)) {
        return;
    }

    count = bulk->read(chunk, sizeof(chunk));
    if (count <= 0) {
        return;
    }

    for (int i = 0; i < count; i++) {
        proto_rx_byte(chunk[i]);
    }
}
