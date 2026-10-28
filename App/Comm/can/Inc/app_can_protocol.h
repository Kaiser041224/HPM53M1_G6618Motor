/**
 * @file    app_can_protocol.h
 * @brief   CAN 报文编解码（CANSimple 语义，SI 派生的 deg/rpm 单位）
 * @author  Kaiser
 *
 * 职责边界：本模块是**纯函数**（不访问参数单例、不驱动外设），便于主机单元测试。
 * ID 布局：(node_id << 5) | cmd_id，标准 11 位帧。
 *
 * 单位（2026-10-27 用户决定）：位置 deg、速度 rpm（出轴）、转矩 Nm。
 * CAN 只承载控制与状态；参数读写/标定/波形走 USB terminal / UART。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_CAN_PROTOCOL_H
#define APP_CAN_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 节点号上限（6 位字段，协议层限制为 1..15） */
#define APP_CAN_NODE_ID_MAX (15U)
/** @brief 未寻址节点号（保持 ODrive 语义：63 = 未寻址） */
#define APP_CAN_NODE_ID_UNADDRESSED (63U)
/** @brief cmd 字段宽度 */
#define APP_CAN_CMD_MASK (0x1FU)

/** @brief 命令 ID */
typedef enum {
    APP_CAN_CMD_HEARTBEAT = 0x001,          /**< 设备→主机：错误/状态/过程结果 */
    APP_CAN_CMD_ESTOP = 0x002,              /**< 主机→设备：急停（立即执行） */
    APP_CAN_CMD_GET_ERROR = 0x003,          /**< 设备→主机：active/latched */
    APP_CAN_CMD_SET_AXIS_STATE = 0x007,     /**< 主机→设备：1=IDLE 8=CLOSED_LOOP */
    APP_CAN_CMD_GET_ENCODER = 0x009,        /**< 设备→主机：连续位置/速度 */
    APP_CAN_CMD_SET_CONTROLLER_MODE = 0x00B,/**< 主机→设备：control_mode/input_mode */
    APP_CAN_CMD_SET_INPUT_POS = 0x00C,      /**< 主机→设备：位置(连续)/前馈 */
    APP_CAN_CMD_SET_INPUT_VEL = 0x00D,      /**< 主机→设备：速度/力矩前馈 */
    APP_CAN_CMD_SET_INPUT_TORQUE = 0x00E,   /**< 主机→设备：力矩 */
    APP_CAN_CMD_SET_LIMITS = 0x00F,         /**< 主机→设备：速度/电流上限（只降不升） */
    APP_CAN_CMD_SET_INPUT_MIT = 0x010,      /**< 主机→设备：MIT 压缩单帧 */
    APP_CAN_CMD_SET_INPUT_MIT_A = 0x011,    /**< 主机→设备：MIT 双帧 A（p/v） */
    APP_CAN_CMD_SET_INPUT_MIT_B = 0x012,    /**< 主机→设备：MIT 双帧 B（kp/kd/t_ff） */
    APP_CAN_CMD_CLEAR_ERRORS = 0x018,       /**< 主机→设备：清除（identify 校验） */
} app_can_cmd_t;

/** @brief Axis 状态（子集） */
#define APP_CAN_AXIS_IDLE        (1U)
#define APP_CAN_AXIS_CLOSED_LOOP (8U)

/** @brief 控制模式（与 ODrive 对齐前三项；MIT 为工程扩展） */
#define APP_CAN_MODE_TORQUE   (1U)
#define APP_CAN_MODE_VELOCITY (2U)
#define APP_CAN_MODE_POSITION (3U)
#define APP_CAN_MODE_MIT      (4U)

/** @brief 解析后的控制帧类型 */
typedef enum {
    APP_CAN_FRAME_NONE = 0,     /**< 非本节点/无需处理 */
    APP_CAN_FRAME_ESTOP,        /**< 急停 */
    APP_CAN_FRAME_AXIS_STATE,   /**< 状态切换请求 */
    APP_CAN_FRAME_CONTROLLER_MODE, /**< 控制模式 */
    APP_CAN_FRAME_INPUT_POS,    /**< 位置命令 */
    APP_CAN_FRAME_INPUT_VEL,    /**< 速度命令 */
    APP_CAN_FRAME_INPUT_TORQUE, /**< 力矩命令 */
    APP_CAN_FRAME_LIMITS,       /**< 限幅 */
    APP_CAN_FRAME_MIT,          /**< MIT 五元组（compact 或 dual-B 配对完成） */
    APP_CAN_FRAME_CLEAR_ERRORS, /**< 清故障 */
    APP_CAN_FRAME_UNKNOWN,      /**< 未知 cmd（计数用） */
} app_can_frame_kind_t;

/** @brief 解码结果 */
typedef struct {
    app_can_frame_kind_t kind;
    union {
        uint8_t axis_state;
        struct {
            uint8_t control_mode;
            uint8_t input_mode;
        } mode;
        struct {
            float pos_deg;
            float vel_ff_rpm;
            float torque_ff_nm;
        } pos;
        struct {
            float vel_rpm;
            float torque_ff_nm;
        } vel;
        float torque_nm;
        struct {
            float vel_limit_rpm;
            float current_limit_a;
        } limits;
        struct {
            float p_deg;
            float v_rpm;
            float kp;
            float kd;
            float t_ff;
        } mit;
        uint32_t identify;
    } u;
} app_can_frame_t;

/** @brief MIT 双帧 A 载荷 */
typedef struct {
    uint8_t seq;   /**< 配对序号 */
    float p_deg;   /**< 目标位置 [deg] */
    float v_rpm;   /**< 目标速度 [rpm] */
} app_can_mit_a_t;

/** @brief MIT 双帧 B 载荷 */
typedef struct {
    uint8_t seq;   /**< 配对序号（须与 A 相同） */
    float kp;      /**< [Nm/rad] */
    float kd;      /**< [Nm/(rad/s)] */
    float t_ff;    /**< [Nm] */
} app_can_mit_b_t;

/**
 * @brief 构造标准帧 ID
 * @param node_id 节点号（1..15；63 = 未寻址）
 * @param cmd 命令 ID
 * @return 11 位 ID
 */
uint32_t app_can_make_id(uint8_t node_id, uint8_t cmd);

/**
 * @brief 拆解 ID
 * @param id 标准帧 ID
 * @param node_id 输出节点号（可为 NULL）
 * @param cmd 输出命令（可为 NULL）
 * @return true = 合法标准帧 ID（≤0x7FF）
 */
bool app_can_parse_id(uint32_t id, uint8_t* node_id, uint8_t* cmd);

/**
 * @brief 解码控制帧
 * @param id 标准帧 ID
 * @param data 载荷
 * @param len 载荷长度（DLC）
 * @param node_id 本机节点号（仅处理匹配帧）
 * @param out 输出
 * @return 0 = 已解析；-1 = 参数非法/长度不足；1 = 非本节点或未知命令
 */
int app_can_decode(uint32_t id, const uint8_t* data, uint8_t len, uint8_t node_id,
                   app_can_frame_t* out);

/**
 * @brief 编码 Heartbeat（8B）
 * @return 载荷长度（8）
 */
uint8_t app_can_encode_heartbeat(uint8_t* data, uint32_t errors, uint8_t axis_state,
                                 uint8_t procedure_result, uint8_t traj_done);

/**
 * @brief 编码 Get_Encoder_Estimates（8B）
 * @return 载荷长度（8）
 */
uint8_t app_can_encode_encoder(uint8_t* data, float pos_deg, float vel_rpm);

/**
 * @brief 编码 Get_Error（8B）
 * @return 载荷长度（8）
 */
uint8_t app_can_encode_error(uint8_t* data, uint32_t active, uint32_t latched);

/**
 * @brief 解码 MIT 双帧 A
 * @return 0 = 成功；-1 = 长度/参数非法
 */
int app_can_decode_mit_a(const uint8_t* data, uint8_t len, app_can_mit_a_t* out);

/**
 * @brief 解码 MIT 双帧 B
 * @return 0 = 成功；-1 = 长度/参数非法
 */
int app_can_decode_mit_b(const uint8_t* data, uint8_t len, app_can_mit_b_t* out);

/**
 * @brief 编码 MIT 压缩单帧（测试/主机侧参考实现，便于往返验证）
 * @return 载荷长度（8）
 */
uint8_t app_can_encode_mit(uint8_t* data, float p_deg, float v_rpm, float kp, float kd,
                           float t_ff, float kp_max, float kd_max);

#ifdef __cplusplus
}
#endif

#endif /* APP_CAN_PROTOCOL_H */