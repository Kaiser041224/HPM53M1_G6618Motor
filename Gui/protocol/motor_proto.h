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
    MP_CMD_STREAM_SUB       = 0x0B, /* 订阅一级流（v1.4 起支持，1kHz） */
    MP_CMD_STREAM_UNSUB     = 0x0C, /* 取消订阅 */
    MP_CMD_PARAM_RESET      = 0x0D, /* v1.3：按名单/全部恢复出厂默认值（独立命令字） */
    MP_CMD_BOOT_INFO        = 0x0E  /* v1.6：active/backup 槽与启动控制状态 */
} mp_cmd_t;

/* v1.4：设备→主机遥测数据帧（非响应，SEQ 为帧计数，主机按 CMD 识别） */
#define MP_CMD_STREAM_DATA (0x8Eu)

/* caps 位（protocol_spec §6.2） */
#define MP_CAP_PARAM_WRITE (1UL << 0)
#define MP_CAP_CONTROL     (1UL << 1)
#define MP_CAP_STREAM      (1UL << 2)
#define MP_CAP_DFU         (1UL << 3) /* v1.6：支持 CTRL_ENTER_DFU / BOOT_INFO */

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
    MP_CTRL_SET_ANGLE            = 0x0A, /* 角度设定（°；负值允许） */
    /* ---- v1.6 增量：进入 DFU ---- */
    MP_CTRL_ENTER_DFU            = 0x0B  /* 要求 FOC OFF；设备复位进 DFU（响应可能丢失） */
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
/* BOOT_INFO req: 空
 * resp: status | running_slot u8 | active_slot u8 | pending_slot u8 | attempts u8
 *              | confirmed u8 | target_slot u8 | target_valid u8 | image_version u32
 *   slot: 0=ACTIVE, 1=BACKUP, 0xFF=无（非槽构建/无记录）；target = active ^ 1（DFU 目标槽）
 *   target_valid = 目标槽镜像头合法（可被覆盖） */
/* PARAM_RESET req: count u8 | [name_len u8 | name]×count（count=0 或空 payload = 全部恢复）
 * resp: status；名单含未知名 → BAD_PARAM；运行中或故障锁存 → LOCKED（flash 类联锁同 COMMIT）；
 *       落盘失败 → BUSY。恢复后值立即生效且持久化（RAM 与落盘语义等同「恢复+写入」）。 */

/* motor_state 取值 */
#define MP_MOTOR_STOP (0x00u)
#define MP_MOTOR_RUN  (0x01u)

/* ---- v1.4 遥测流（STREAM_SUB）----
 *
 * 订阅请求：rate_hz u16 | batch u16（rate 必须 1000；batch 0 = 默认 7，范围 1..7）
 * 数据帧：CMD = MP_CMD_STREAM_DATA，SEQ = 帧计数（u8 回绕），payload =
 *         mp_stream_header_t + count × mp_telemetry_sample_t
 * 样本固定 72 字节、无 padding、全小端；主机必须按 sample_size 校验。
 */
typedef struct {
    uint32_t sample_index; /**< 本帧首样本序号（1kHz 递增，u32 回绕 ~49 天） */
    uint16_t count;        /**< 本帧样本数 1..7 */
    uint8_t  sample_size;  /**< sizeof(mp_telemetry_sample_t) = 72 */
    uint8_t  reserved;     /**< 0 */
} mp_stream_header_t;      /* 8 字节 */

typedef struct {
    uint32_t t_us;          /**< 采样时刻 [µs]（u32 回绕 ~71.6 min，主机按差值展开） */
    float i_u_a;            /**< U 相电流 [A] */
    float i_v_a;            /**< V 相电流 [A] */
    float i_w_a;            /**< W 相电流 [A] */
    float v_bus_v;          /**< 母线电压 [V] */
    float ntc0_c;           /**< NTC0 温度 [℃]（无效 = 0） */
    float ntc1_c;           /**< NTC1 温度 [℃]（无效 = 0） */
    float r_ntc0_ohm;       /**< NTC0 原始电阻 [Ω] */
    float r_ntc1_ohm;       /**< NTC1 原始电阻 [Ω] */
    float enc_rotor_deg;    /**< 转子机械角 [°]（已应用软件零点；读失败 = 0） */
    float enc_output_deg;   /**< 输出轴角 [°]（已应用零点/方向；读失败 = 0） */
    float i_d_a;            /**< d 轴反馈 [A] */
    float i_q_a;            /**< q 轴反馈 [A] */
    float speed_rad_s;      /**< 轴端速度 [rad/s]（observer） */
    float torque_nm;        /**< motion 转矩请求 [Nm] */
    float pos_rad;          /**< 输出轴位置 [rad]（motion 内部坐标） */
    uint32_t fault_flags;   /**< 实时故障位 */
    uint16_t enc_rotor_raw; /**< 转子原始码（16bit） */
    uint16_t enc_output_raw;/**< 游标原始码（16bit） */
} mp_telemetry_sample_t;    /* 72 字节 */

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_PROTO_H */
