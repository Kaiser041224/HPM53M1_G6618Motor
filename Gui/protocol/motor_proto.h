/**
 * @file    motor_proto.h
 * @brief   Gui <-> 电机控制板 通讯协议 v1 双端契约（线格式唯一事实源）
 * @author  Kaiser
 *
 * 字节序：全部小端。帧格式：
 *   | SOF 0xA55A | VER(1) | CMD(1) | SEQ(1) | LEN(2) | PAYLOAD(0..512) | CRC16(2) |
 * CRC16-CCITT-FALSE：init 0xFFFF, poly 0x1021, 不反转不取反，覆盖 VER..PAYLOAD。
 * 响应 CMD = 请求 CMD | 0x80；响应 PAYLOAD 首字节 = mp_status_t，其后为命令体。
 * 请求/响应以 SEQ 配对（u8 回绕）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef MOTOR_PROTO_H
#define MOTOR_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP_SOF0            (0xA5u)
#define MP_SOF1            (0x5Au)
#define MP_VERSION         (0x01u)
#define MP_MAX_PAYLOAD     (512u)
#define MP_RESPONSE_BIT    (0x80u)

/* 命令字（请求方向）；响应 = CMD | MP_RESPONSE_BIT */
typedef enum {
    MP_CMD_PROTO_INFO       = 0x01, /* 版本协商/能力位/设备名 */
    MP_CMD_PARAM_META_COUNT = 0x02, /* 元数据条目数 */
    MP_CMD_PARAM_META_GET   = 0x03, /* 按序号取元数据 */
    MP_CMD_PARAM_READ       = 0x04, /* 按名读参数 */
    MP_CMD_PARAM_WRITE      = 0x05, /* 按名写参数 */
    MP_CMD_PARAM_COMMIT     = 0x06, /* 参数落盘（停机窗口联锁） */
    MP_CMD_STATUS_GET       = 0x07, /* 运行状态快照 */
    MP_CMD_CTRL_CMD         = 0x08, /* 电机控制（联锁同固件 terminal） */
    MP_CMD_FAULT_GET        = 0x09, /* 故障与锁存 */
    MP_CMD_FAULT_CLEAR      = 0x0A, /* 清除故障锁存 */
    MP_CMD_STREAM_SUB       = 0x0B, /* 订阅一级流（v1 预留，返回 UNSUPPORTED） */
    MP_CMD_STREAM_UNSUB     = 0x0C, /* 取消订阅（v1 预留） */
    MP_CMD_PARAM_RESET      = 0x0D  /* v1.3：按名单/全部恢复出厂默认值（独立命令字） */
} mp_cmd_t;

/* 响应状态码（响应 payload 首字节） */
typedef enum {
    MP_STATUS_OK          = 0x00,
    MP_STATUS_BAD_CMD     = 0x01,
    MP_STATUS_BAD_PARAM   = 0x02, /* 名字不存在/类型不符 */
    MP_STATUS_BUSY        = 0x03,
    MP_STATUS_LOCKED      = 0x04, /* 联锁拒绝：故障锁存或未停机 */
    MP_STATUS_UNSUPPORTED = 0x05,
    MP_STATUS_RANGE_ERR   = 0x06  /* 数值越界 */
} mp_status_t;

/* 参数类型 / 生效语义（PARAM_META_* 响应体） */
typedef enum { MP_TYPE_F32 = 0, MP_TYPE_U32 = 1, MP_TYPE_I32 = 2, MP_TYPE_BOOL = 3 } mp_type_t;
typedef enum { MP_APPLY_LIVE = 0, MP_APPLY_REBOOT = 1 } mp_apply_t;

/* CTRL_CMD opcode（请求体首字节，后跟 f32 value）
 *
 * v1.1 增量（additive，固件侧待 M2 实现；旧端对下列 opcode 返回 BAD_CMD）：
 *   MP_CTRL_SET_OUTPUT_ZERO_NOW (0x05)：把“当前出轴角”设为零点，value 忽略（传 0）。
 *       语义：enc_output_deg 显示自此偏移归零。**仅出轴**——转子零点不在此命令，
 *       属电机·驱动域（由驱动/编码器侧单独处理）。
 *   MP_CTRL_SET_OUTPUT_ZERO_ABS (0x06)：value = 指定角度（度），把该角度设为出轴零点。
 *       非有限值（NaN/±Inf）返回 MP_STATUS_RANGE_ERR。
 *   MP_CTRL_SET_MODE (0x07)：value 以 f32 位型低字节承载 u8 模式（1=力矩 / 2=速度 /
 *       3=角度）；非法值返回 MP_STATUS_BAD_CMD。模式仅存设备状态，STATUS 布局不变、
 *       无回读字段（UI 侧自行记录）。
 * 联锁：零点写入与模式切换为停机窗口操作——停机且无故障锁存时 OK，运行中或存在故障
 *       锁存时 MP_STATUS_LOCKED。
 *
 * v1.2 增量（additive，固件侧待 M2 实现；旧端对下列 opcode 返回 BAD_CMD）：
 *   MP_CTRL_SET_TORQUE (0x08)：value = 力矩设定（N·m）。非有限值→RANGE_ERR；负值允许
 *       （回馈/反转语义）。联锁同 v1.1（停机窗口）。
 *   MP_CTRL_SET_SPEED (0x09)：value = 转速设定（rpm）。|value| > 20000 → RANGE_ERR；
 *       负值允许。联锁同 v1.1。
 *   MP_CTRL_SET_ANGLE (0x0A)：value = 角度设定（°）。非有限值→RANGE_ERR；负值允许。
 *       联锁同 v1.1。
 * 响应语义同 v1.1：停机且无故障锁存时 OK；运行中或存在故障锁存时 LOCKED
 *   （从严保持一致：速度设定亦不允许运行中热改，须先停机改设定再启动）。
 */
typedef enum {
    MP_CTRL_START                = 0x01,
    MP_CTRL_STOP                 = 0x02,
    MP_CTRL_SET_FREQ             = 0x03,
    MP_CTRL_SET_MOD              = 0x04,
    /* ---- v1.1 增量 ---- */
    MP_CTRL_SET_OUTPUT_ZERO_NOW  = 0x05, /* 当前出轴角归零；value 忽略 */
    MP_CTRL_SET_OUTPUT_ZERO_ABS  = 0x06, /* 指定角度(度)设为出轴零点 */
    MP_CTRL_SET_MODE             = 0x07, /* 模式：1=力矩/2=速度/3=角度 */
    /* ---- v1.2 增量：设定值写入（联锁同 v1.1，停机窗口） ---- */
    MP_CTRL_SET_TORQUE           = 0x08, /* 力矩设定（N·m；负值允许） */
    MP_CTRL_SET_SPEED            = 0x09, /* 转速设定（rpm；|v|>20000 越界；负值允许） */
    MP_CTRL_SET_ANGLE            = 0x0A  /* 角度设定（°；负值允许） */
} mp_ctrl_opcode_t;

/* 帧头布局（CRC 覆盖 VER..PAYLOAD，即 5 字节头 + PAYLOAD） */
typedef struct {
    uint8_t  ver;      /* MP_VERSION */
    uint8_t  cmd;
    uint8_t  seq;
    uint16_t len;      /* PAYLOAD 字节数，0..MP_MAX_PAYLOAD */
} mp_hdr_t;            /* 线上 5 字节，逐字段小端序列化，勿直接 memcpy */

/* --- 命令体布局（响应体在首部含 mp_status_t = uint8_t）--- */

/* PROTO_INFO resp: status | proto_major u8 | proto_minor u8 | caps u32 | name_len u8 | name[name_len] */
/* PARAM_META_COUNT resp: status | count u16 */
/* PARAM_META_GET req: index u16
 * resp: status | index u16 | type u8 | apply u8 | min f32 | max f32 | name_len u8 | name | unit_len u8 | unit */
/* PARAM_READ req: name_len u8 | name; resp: status | type u8 | value u32（原始位型，按 type 重解释） */
/* PARAM_WRITE req: name_len u8 | name | type u8 | value u32; resp: status */
/* PARAM_COMMIT req: 空; resp: status */
/* STATUS_GET resp: status | fault_flags u32 | motor_running u8 | motor_state u8 | freq_hz f32 | mod_pct f32
 *                | i_u f32 | i_v f32 | i_w f32 | vbus f32 | ntc_temp f32 | enc_rotor_deg f32 | enc_output_deg f32 */
/* CTRL_CMD req: opcode u8 | value f32; resp: status */
/* FAULT_GET resp: status | fault_flags u32 | latched_flags u32 */
/* FAULT_CLEAR req: 空; resp: status */
/* PARAM_RESET req: count u8 | [name_len u8 | name]×count（count=0 或空 payload = 全部恢复）
 * resp: status；名单含未知名 → BAD_PARAM；运行中或故障锁存 → LOCKED（flash 类联锁同 COMMIT）；
 *       落盘失败 → BUSY。恢复后值立即生效且持久化（RAM 与落盘语义等同「恢复+写入」）。 */

/* motor_state 取值 */
#define MP_MOTOR_STOP (0x00u)
#define MP_MOTOR_RUN  (0x01u)

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_PROTO_H */
