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
#include "app_hardware_params.h"
#include "app_motion.h"
#include "app_motor_params.h"
#include "app_param_meta.h"
#include "app_software_params.h"
#include "app_usb.h"
#include "foc_math.h"
#include "intf_clock.h"
#include "motor_proto.h"

#include <string.h>

/** 设备名（PROTO_INFO 返回；≤ 32 字节） */
#define PROTO_DEVICE_NAME     "HPM53M1"
#define PROTO_DEVICE_NAME_LEN (sizeof(PROTO_DEVICE_NAME) - 1U)

/** 每拍从接收环读取的上限 [byte] */
#define PROTO_SERVICE_READ_MAX (256U)

/** 参数名最大长度（含结尾 NUL 的缓冲为 64） */
#define PROTO_NAME_MAX (63U)

/* 能力位（protocol_spec §6.2）：bit0=PARAM_WRITE，bit1=CONTROL，bit2=STREAM */
#define PROTO_CAPS (MP_CAP_PARAM_WRITE | MP_CAP_CONTROL | MP_CAP_STREAM)

/* bulk 写一律非阻塞：本服务跑在 1kHz io 任务内，任何毫秒级阻塞都会推迟
 * 同任务的故障 tick / 终端处理。忙则丢帧（应答进 pending 重试，流直接丢）。 */
#define PROTO_TX_TIMEOUT_MS (0U)

/* 遥测：1kHz 抽点，batch 上限 7（8B 头 + 7×72B = 512B = MP_MAX_PAYLOAD） */
#define PROTO_STREAM_MAX_BATCH   (7U)
#define PROTO_STREAM_DEFAULT_BATCH (7U)
#define PROTO_STREAM_RATE_HZ     (1000U)

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

/* 应答重试槽：端点忙时暂存一帧，下一拍重发；新应答覆盖旧帧（主机已超时） */
static uint8_t s_pending_frame[2U + 5U + PROTO_RESP_PAYLOAD_MAX + 2U];
static uint16_t s_pending_len;

/* 遥测批缓冲（1kHz io 任务独占，生产与消费同线程，无并发） */
static struct {
    bool active;          /**< 是否已订阅 */
    uint16_t batch;       /**< 每帧样本数 1..7 */
    uint8_t count;        /**< 当前批已积累样本数 */
    uint32_t next_index;  /**< 下一帧首样本序号 */
    uint32_t drops;       /**< 端点忙/未就绪丢弃的批数（诊断） */
    mp_telemetry_sample_t samples[PROTO_STREAM_MAX_BATCH];
} s_stream;

/* 32bit cycle → 64bit 扩展（1kHz 调用；8.94s 回绕远大于采样间隔） */
static uint32_t s_cycle_last;
static uint64_t s_cycle_hi;

_Static_assert(sizeof(mp_telemetry_sample_t) == 72U, "telemetry sample layout changed");
_Static_assert(sizeof(mp_stream_header_t) == 8U, "stream header layout changed");

/* 前向声明：遥测组帧先于通用组帧定义使用 */
static uint16_t proto_build_frame(uint8_t cmd_byte, uint8_t seq, const uint8_t *payload,
                                  uint16_t len);

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
 * 遥测流（1kHz；数据全部取自已有生产者，不在 ISR 内新增采样）
 * ============================================================================ */

/**
 * @brief 读取并扩展 32bit cycle 计数（每 1ms 调用一次，可跨 8.94s 回绕）
 * @return 64bit cycle
 */
static uint64_t proto_now_cycles(void) {
    uint32_t cycles = intf_clock_get_cycle();

    if (cycles < s_cycle_last) {
        s_cycle_hi += (1ULL << 32);
    }
    s_cycle_last = cycles;
    return s_cycle_hi | (uint64_t) cycles;
}

/**
 * @brief 单调微秒时间戳
 * @return [µs]（u32，回绕约 71.6 min，主机按差值展开）
 */
static uint32_t proto_now_us(void) {
    uint32_t freq = intf_clock_get_cpu_freq();

    if (freq == 0U) {
        return 0U;
    }
    return (uint32_t) ((proto_now_cycles() * 1000000ULL) / freq);
}

/**
 * @brief 采集一个遥测样本（1kHz io 任务；未订阅时不产生任何读取）
 */
static void proto_telemetry_sample(void) {
    mp_telemetry_sample_t *sample;
    app_analog_values_t analog;
    app_encoder_rotor_snapshot_t pair;
    app_foc_current_snapshot_t foc;
    app_motion_snapshot_t motion;
    uint16_t output_raw = 0U;
    float rotor_deg = 0.0f;
    float output_deg = 0.0f;
    float ntc0_c = 0.0f;
    float ntc1_c = 0.0f;

    if (!s_stream.active) {
        return;
    }
    if (!app_usb_bulk_ready()) {
        s_stream.count = 0U; /* 主机不在：丢弃未满批 */
        return;
    }

    (void) app_analog_signal_read_all(&analog);
    (void) app_encoder_output_sample(&pair, &output_raw);
    (void) app_encoder_read_deg(APP_ENCODER_ROTOR, &rotor_deg);
    (void) app_encoder_read_deg(APP_ENCODER_OUTPUT, &output_deg);
    (void) app_analog_signal_ntc_temp_c(ADC_CH_NTC0, &ntc0_c);
    (void) app_analog_signal_ntc_temp_c(ADC_CH_NTC1, &ntc1_c);
    app_foc_current_get_snapshot(&foc);
    app_motion_get_snapshot(&motion);

    sample = &s_stream.samples[s_stream.count];
    sample->t_us = proto_now_us();
    sample->i_u_a = analog.i_u_a;
    sample->i_v_a = analog.i_v_a;
    sample->i_w_a = analog.i_w_a;
    sample->v_bus_v = analog.v_bus_v;
    sample->ntc0_c = ntc0_c;
    sample->ntc1_c = ntc1_c;
    sample->r_ntc0_ohm = analog.r_ntc0_ohm;
    sample->r_ntc1_ohm = analog.r_ntc1_ohm;
    sample->enc_rotor_deg = rotor_deg;
    sample->enc_output_deg = output_deg;
    sample->i_d_a = foc.i_d_a;
    sample->i_q_a = foc.i_q_a;
    sample->speed_rad_s = foc.speed_out_rad_s;
    sample->torque_nm = motion.torque_nm;
    sample->pos_rad = motion.shaft_rad;
    sample->fault_flags = app_fault_get_codes();
    sample->enc_rotor_raw = pair.raw;
    sample->enc_output_raw = output_raw;

    s_stream.count++;
}

/**
 * @brief 满批则组帧发送（非阻塞；失败只计丢弃，不重试遥测）
 */
static void proto_stream_flush(void) {
    uint16_t payload_len;
    uint16_t frame_len;

    if (!s_stream.active || (s_stream.count == 0U) || (s_stream.count < s_stream.batch)) {
        return;
    }

    payload_len = (uint16_t) (sizeof(mp_stream_header_t)
                              + ((uint32_t) s_stream.count * sizeof(mp_telemetry_sample_t)));

    put_u32(&s_resp_payload[0], s_stream.next_index);
    put_u16(&s_resp_payload[4], s_stream.count);
    s_resp_payload[6] = (uint8_t) sizeof(mp_telemetry_sample_t);
    s_resp_payload[7] = 0U;
    memcpy(&s_resp_payload[sizeof(mp_stream_header_t)], s_stream.samples,
           (size_t) s_stream.count * sizeof(mp_telemetry_sample_t));

    frame_len = proto_build_frame(MP_CMD_STREAM_DATA, (uint8_t) (s_stream.next_index & 0xFFU),
                                  s_resp_payload, payload_len);
    if (app_usb_bulk_write(s_tx_frame, frame_len, PROTO_TX_TIMEOUT_MS) != 0) {
        s_stream.drops++;
    }

    s_stream.next_index += s_stream.count;
    s_stream.count = 0U;
}

/* ============================================================================
 * P1b：参数写 / 落盘 / 恢复出厂 / 控制
 * ============================================================================ */

/**
 * @brief 线格式 u32 → 用于范围比较的 float
 */
static float proto_value_as_float(uint8_t meta_type, uint32_t raw) {
    union {
        float f;
        uint32_t u;
    } bits;

    switch (meta_type) {
    case PARAM_META_TYPE_U8: return (float) (uint8_t) raw;
    case PARAM_META_TYPE_U16: return (float) (uint16_t) raw;
    case PARAM_META_TYPE_U32: return (float) raw;
    default:
        bits.u = raw;
        return bits.f;
    }
}

/**
 * @brief 线格式 u32 → 参数存储（按元数据类型截断/位型重解释）
 */
static void proto_store_raw(uint8_t meta_type, void *data, uint32_t raw) {
    union {
        float f;
        uint32_t u;
    } bits;

    switch (meta_type) {
    case PARAM_META_TYPE_U8: *(uint8_t *) data = (uint8_t) raw; break;
    case PARAM_META_TYPE_U16: *(uint16_t *) data = (uint16_t) raw; break;
    case PARAM_META_TYPE_U32: *(uint32_t *) data = raw; break;
    default:
        bits.u = raw;
        *(float *) data = bits.f;
        break;
    }
}

/**
 * @brief 是否为编码器零点/方向参数（改动需清「已标定」标志，同终端）
 */
static bool proto_is_encoder_calib_param(const char *name) {
    return (strcmp(name, "motor.encoder.electrical_offset_rad") == 0)
           || (strcmp(name, "motor.encoder.direction") == 0);
}

/**
 * @brief 从 payload 解出 len-prefixed 名称（写入 s_name）
 * @return true = 成功
 */
static bool proto_read_name(const uint8_t *payload, uint16_t len, uint16_t *pos) {
    uint8_t name_len;

    if (*pos >= len) {
        return false;
    }
    name_len = payload[*pos];
    if ((name_len == 0U) || (name_len > PROTO_NAME_MAX)
        || ((uint16_t) (*pos + 1U + name_len) > len)) {
        return false;
    }
    memcpy(s_name, &payload[*pos + 1U], name_len);
    s_name[name_len] = '\0';
    *pos = (uint16_t) (*pos + 1U + name_len);
    return true;
}

/**
 * @brief PARAM_WRITE：name_len | name | type u8 | value u32
 */
static uint16_t proto_param_write(const uint8_t *payload, uint16_t len, uint8_t *out) {
    const param_meta_t *meta;
    void *data;
    uint16_t pos;
    uint8_t wire_type;
    uint32_t raw;
    float value;

    if (len < 1U) {
        return proto_status_only(out, MP_STATUS_BAD_PARAM);
    }
    pos = 0U;
    if (!proto_read_name(payload, len, &pos)) {
        return proto_status_only(out, MP_STATUS_BAD_PARAM);
    }
    if ((uint16_t) (pos + 5U) > len) {
        return proto_status_only(out, MP_STATUS_BAD_PARAM);
    }
    wire_type = payload[pos];
    raw = (uint32_t) payload[pos + 1U] | ((uint32_t) payload[pos + 2U] << 8)
          | ((uint32_t) payload[pos + 3U] << 16) | ((uint32_t) payload[pos + 4U] << 24);

    meta = app_param_meta_find(s_name);
    if (meta == NULL) {
        return proto_status_only(out, MP_STATUS_BAD_PARAM);
    }
    if (wire_type != proto_type(meta->type)) {
        return proto_status_only(out, MP_STATUS_BAD_PARAM);
    }
    if ((meta->apply == PARAM_META_APPLY_FOC_OFF)
        && (app_foc_get_state() != APP_FOC_STATE_OFF)) {
        return proto_status_only(out, MP_STATUS_LOCKED);
    }

    value = proto_value_as_float(meta->type, raw);
    if ((meta->type == PARAM_META_TYPE_F32) && !foc_finite(value)) {
        return proto_status_only(out, MP_STATUS_RANGE_ERR);
    }
    if ((meta->min <= meta->max) && ((value < meta->min) || (value > meta->max))) {
        return proto_status_only(out, MP_STATUS_RANGE_ERR);
    }

    data = app_param_meta_data(meta);
    if (data == NULL) {
        return proto_status_only(out, MP_STATUS_BUSY);
    }
    proto_store_raw(meta->type, data, raw);

    if (proto_is_encoder_calib_param(s_name)) {
        app_motor_params_mutable()->encoder.electrical_offset_valid = 0U;
    }
    return proto_status_only(out, MP_STATUS_OK);
}

/**
 * @brief PARAM_COMMIT：镜像终端 param save（FOC OFF + 非停机中；hardware 不持久化）
 */
static uint16_t proto_param_commit(uint8_t *out) {
    if (app_foc_stop_pending() || (app_foc_get_state() != APP_FOC_STATE_OFF)) {
        return proto_status_only(out, MP_STATUS_LOCKED);
    }
    if ((app_motor_params_store() != 0) || (app_software_params_store() != 0)) {
        return proto_status_only(out, MP_STATUS_BUSY);
    }
    return proto_status_only(out, MP_STATUS_OK);
}

/**
 * @brief PARAM_RESET：count u8 | [name_len | name]×count；count=0 = 全部
 *
 * 与终端的差异（协议 spec 约定）：本命令恢复后立即落盘；终端 reset 只改 RAM。
 */
static uint16_t proto_param_reset(const uint8_t *payload, uint16_t len, uint8_t *out) {
    uint8_t count = 0U;
    uint16_t pos = 1U;

    if (app_foc_get_state() != APP_FOC_STATE_OFF) {
        return proto_status_only(out, MP_STATUS_LOCKED);
    }
    if (len > 0U) {
        count = payload[0];
    }

    if (count == 0U) {
        *app_motor_params_mutable() = *app_motor_params_default();
        *app_hardware_params_mutable() = *app_hardware_params_default();
        *app_software_params_mutable() = *app_software_params_default();
        app_motor_params_mutable()->encoder.electrical_offset_valid = 0U;
    } else {
        uint16_t scan = pos;

        /* 先整体校验，避免未知名导致部分恢复 */
        for (uint8_t i = 0U; i < count; i++) {
            if (!proto_read_name(payload, len, &scan)
                || (app_param_meta_find(s_name) == NULL)) {
                return proto_status_only(out, MP_STATUS_BAD_PARAM);
            }
        }

        scan = pos;
        for (uint8_t i = 0U; i < count; i++) {
            const param_meta_t *meta;
            void *data;

            if (!proto_read_name(payload, len, &scan)) {
                return proto_status_only(out, MP_STATUS_BAD_PARAM);
            }
            meta = app_param_meta_find(s_name);
            data = app_param_meta_data(meta);
            if ((meta == NULL) || (data == NULL)) {
                return proto_status_only(out, MP_STATUS_BAD_PARAM);
            }
            memcpy(data, app_param_meta_factory_data(meta),
                   app_param_meta_type_size(meta->type));
            if (proto_is_encoder_calib_param(s_name)) {
                app_motor_params_mutable()->encoder.electrical_offset_valid = 0U;
            }
        }
    }

    if ((app_motor_params_store() != 0) || (app_software_params_store() != 0)) {
        return proto_status_only(out, MP_STATUS_BUSY);
    }
    return proto_status_only(out, MP_STATUS_OK);
}

/**
 * @brief motion 返回码 → 协议状态
 */
static uint8_t proto_motion_status(int rc) {
    if (rc == APP_MOTION_CMD_OK) {
        return MP_STATUS_OK;
    }
    if (rc == APP_MOTION_CMD_BAD_PARAM) {
        return MP_STATUS_BAD_PARAM;
    }
    if (rc == APP_MOTION_CMD_TARGET_LIMIT) {
        return MP_STATUS_RANGE_ERR;
    }
    return MP_STATUS_LOCKED;
}

/**
 * @brief CTRL_CMD：opcode u8 | value f32
 *
 * 映射（v1.5）：
 *   START      → app_foc_enable()（含约 10ms 栅极供电稳定阻塞）
 *   STOP       → app_foc_request_stop()（非阻塞；FAULT 态的 disable 分支除外）
 *   SET_MODE   → 1=力矩/2=速度/3=角度；跨模式先 release_output，下一设定值进入新模式
 *   SET_SPEED  → app_motion_speed(rpm × 2π/60)     [轴端 rad/s，同终端 motion speed]
 *   SET_TORQUE → app_motion_torque(Nm)
 *   SET_ANGLE  → app_motion_position(deg × π/180, 0, 0)
 *   SET_FREQ/SET_MOD/SET_OUTPUT_ZERO_* → UNSUPPORTED（本机无 V/F；零点写入需停机 job）
 *
 * 联锁：设定值允许运行中改；FOC OFF 限制只作用于参数 apply=foc_off 与 flash 类操作。
 */
static uint16_t proto_ctrl(const uint8_t *payload, uint16_t len, uint8_t *out) {
    union {
        float f;
        uint32_t u;
    } bits;
    uint8_t op;
    float value;

    if (len < 5U) {
        return proto_status_only(out, MP_STATUS_BAD_PARAM);
    }
    op = payload[0];
    bits.u = (uint32_t) payload[1] | ((uint32_t) payload[2] << 8)
             | ((uint32_t) payload[3] << 16) | ((uint32_t) payload[4] << 24);
    value = bits.f;

    switch (op) {
    case MP_CTRL_START:
        return proto_status_only(out,
                                 (app_foc_enable() == 0) ? MP_STATUS_OK : MP_STATUS_LOCKED);

    case MP_CTRL_STOP:
        return proto_status_only(out,
                                 (app_foc_request_stop() == 0) ? MP_STATUS_OK : MP_STATUS_LOCKED);

    case MP_CTRL_SET_MODE: {
        uint8_t mode = (uint8_t) (bits.u & 0xFFU);
        app_motion_snapshot_t snap;

        if ((mode < 1U) || (mode > 3U)) {
            return proto_status_only(out, MP_STATUS_BAD_PARAM);
        }
        app_motion_get_snapshot(&snap);
        if (snap.mode != (app_motion_mode_t) (mode - 1U)) {
            (void) app_motion_release_output();
        }
        return proto_status_only(out, MP_STATUS_OK);
    }

    case MP_CTRL_SET_TORQUE:
        if (!foc_finite(value)) {
            return proto_status_only(out, MP_STATUS_RANGE_ERR);
        }
        return proto_status_only(out, proto_motion_status(app_motion_torque(value)));

    case MP_CTRL_SET_SPEED:
        if (!foc_finite(value) || (value > 20000.0f) || (value < -20000.0f)) {
            return proto_status_only(out, MP_STATUS_RANGE_ERR);
        }
        return proto_status_only(
            out, proto_motion_status(app_motion_speed(value * 0.10471975512f)));

    case MP_CTRL_SET_ANGLE:
        if (!foc_finite(value)) {
            return proto_status_only(out, MP_STATUS_RANGE_ERR);
        }
        return proto_status_only(
            out, proto_motion_status(app_motion_position(value * 0.01745329252f, 0.0f, 0.0f)));

    default:
        /* SET_FREQ / SET_MOD / SET_OUTPUT_ZERO_NOW / SET_OUTPUT_ZERO_ABS */
        return proto_status_only(out, MP_STATUS_UNSUPPORTED);
    }
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

    /* v1.4：遥测流订阅（只支持 1kHz；batch 0 = 默认 7） */
    case MP_CMD_STREAM_SUB: {
        uint16_t rate;
        uint16_t batch;

        if (len < 4U) {
            return proto_status_only(out, MP_STATUS_BAD_PARAM);
        }
        rate = (uint16_t) (payload[0] | ((uint16_t) payload[1] << 8));
        batch = (uint16_t) (payload[2] | ((uint16_t) payload[3] << 8));
        if (rate != PROTO_STREAM_RATE_HZ) {
            return proto_status_only(out, MP_STATUS_RANGE_ERR);
        }
        if (batch == 0U) {
            batch = PROTO_STREAM_DEFAULT_BATCH;
        }
        if (batch > PROTO_STREAM_MAX_BATCH) {
            return proto_status_only(out, MP_STATUS_RANGE_ERR);
        }
        s_stream.active = true;
        s_stream.batch = batch;
        s_stream.count = 0U;
        s_stream.next_index = 0U;
        return proto_status_only(out, MP_STATUS_OK);
    }

    case MP_CMD_STREAM_UNSUB:
        s_stream.active = false;
        s_stream.count = 0U;
        return proto_status_only(out, MP_STATUS_OK);

    case MP_CMD_PARAM_WRITE:
        return proto_param_write(payload, len, out);

    case MP_CMD_PARAM_COMMIT:
        return proto_param_commit(out);

    case MP_CMD_PARAM_RESET:
        return proto_param_reset(payload, len, out);

    case MP_CMD_CTRL_CMD:
        return proto_ctrl(payload, len, out);

    default:
        return proto_status_only(out, MP_STATUS_BAD_CMD);
    }
}

/**
 * @brief 组帧（SOF + 5 字节头 + payload + CRC）
 * @return 帧总长度
 */
static uint16_t proto_build_frame(uint8_t cmd_byte, uint8_t seq, const uint8_t *payload,
                                  uint16_t len) {
    uint16_t crc = 0xFFFFU;
    uint16_t pos = 0U;

    s_tx_frame[pos++] = MP_SOF0;
    s_tx_frame[pos++] = MP_SOF1;

    s_tx_frame[pos] = MP_VERSION;
    crc = crc16_update(crc, s_tx_frame[pos++]);
    s_tx_frame[pos] = cmd_byte;
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

    payload_len = proto_dispatch(s_rx_cmd, s_rx_payload, s_rx_len, s_resp_payload);
    frame_len = proto_build_frame((uint8_t) (s_rx_cmd | MP_RESPONSE_BIT), s_rx_seq,
                                  s_resp_payload, payload_len);

    if (app_usb_bulk_write(s_tx_frame, frame_len, PROTO_TX_TIMEOUT_MS) != 0) {
        /* 端点忙：暂存一帧，下一拍重发（遥测流优先级低于应答） */
        memcpy(s_pending_frame, s_tx_frame, frame_len);
        s_pending_len = frame_len;
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

    s_pending_len = 0U;
    s_stream.active = false;
    s_stream.batch = PROTO_STREAM_DEFAULT_BATCH;
    s_stream.count = 0U;
    s_stream.next_index = 0U;
    s_stream.drops = 0U;

    s_cycle_hi = 0U;
    s_cycle_last = intf_clock_get_cycle();
}

void app_usb_proto_service(void) {
    uint8_t chunk[PROTO_SERVICE_READ_MAX];
    int count;
    int i;

    /* 0. 保持 32bit cycle 扩展跟踪（流未订阅时也要跨回绕） */
    (void) proto_now_cycles();

    /* 1. 重发上一拍因端点忙暂存的应答 */
    if ((s_pending_len != 0U)
        && (app_usb_bulk_write(s_pending_frame, s_pending_len, PROTO_TX_TIMEOUT_MS) == 0)) {
        s_pending_len = 0U;
    }

    /* 2. 处理命令请求（每拍最多 PROTO_SERVICE_READ_MAX 字节） */
    count = app_usb_bulk_read(chunk, sizeof(chunk));
    if (count > 0) {
        for (i = 0; i < count; i++) {
            proto_rx_byte(chunk[i]);
        }
    }

    /* 3. 遥测：抽点 + 满批发送 */
    proto_telemetry_sample();
    proto_stream_flush();
}
