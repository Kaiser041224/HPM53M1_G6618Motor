/**
 * @file    app_can_protocol.h
 * @brief   DM 兼容 CAN 协议纯编解码（无状态、无硬件/参数依赖）
 * @author  Kaiser
 *
 * ID：`0x100 | (type << 4) | node`，标准帧；type 1..8，node 0..15。
 *   type 1 = MIT 控制（8B）      2 = 位置速度（8B）
 *        3 = 速度（4..8B）       4 = 力位混控（8B）
 *        5 = 使能（magic 0xFC）  6 = 失能（magic 0xFD）
 *        7 = 清故障（magic 0xFE） 8 = 反馈（电机→主机）
 *
 * 载荷格式按 DM 手册（大端位序、float32 小端）；线缆定标为编译期契约：
 * P_MAX=12.5 rad、V_MAX=30 rad/s、KP_MAX=500、KD_MAX=5；
 * MIT t_ff 保持 DM 的 ±10 Nm，反馈 T 使用本电机全量程 ±200 Nm。
 * 这些是**线缆契约**，不是本机能力上限；本机限幅仍在 motion/FOC 层。
 *
 * 职责边界：本模块只做字节 ↔ 物理量转换，不读参数单例、不驱动外设、不做策略。
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

/** @brief ID 前缀（(id >> 8) == 0x1 即属于本方案） */
#define APP_CAN_DM_ID_BASE (0x100U)
/** @brief 反馈 ID 前缀 */
#define APP_CAN_DM_FEEDBACK_BASE (0x180U)
/** @brief 节点号上限（4 位字段） */
#define APP_CAN_DM_NODE_MAX (15U)

/** @brief 帧类型 */
#define APP_CAN_DM_TYPE_MIT       (1U)
#define APP_CAN_DM_TYPE_POS_VEL   (2U)
#define APP_CAN_DM_TYPE_VEL       (3U)
#define APP_CAN_DM_TYPE_FORCE_POS (4U)
#define APP_CAN_DM_TYPE_ENABLE    (5U)
#define APP_CAN_DM_TYPE_DISABLE   (6U)
#define APP_CAN_DM_TYPE_CLEAR     (7U)
#define APP_CAN_DM_TYPE_FEEDBACK  (8U)

/** @brief 扩展帧 magic（防止总线上全 0 帧误动作） */
#define APP_CAN_DM_MAGIC_ENABLE  (0xFCU)
#define APP_CAN_DM_MAGIC_DISABLE (0xFDU)
#define APP_CAN_DM_MAGIC_CLEAR   (0xFEU)

/** @brief 线缆定标：控制帧与 DM 手册一致；反馈转矩为本电机扩展量程 */
#define APP_CAN_DM_P_MAX_RAD   (12.5f)
#define APP_CAN_DM_V_MAX_RAD_S (30.0f)
#define APP_CAN_DM_T_MAX_NM    (10.0f)  /**< MIT t_ff 线缆（DM 兼容） */
#define APP_CAN_DM_T_FB_MAX_NM (200.0f) /**< 反馈 T 线缆（覆盖 30 A→175.5 Nm，留裕量） */
#define APP_CAN_DM_KP_MAX      (500.0f)
#define APP_CAN_DM_KD_MAX      (5.0f)

/** @brief 温度无效哨兵（反馈 D6/D7） */
#define APP_CAN_DM_TEMP_INVALID (0xFFU)

/** @brief 解码后的帧类别 */
typedef enum {
    APP_CAN_DM_KIND_NONE = 0, /**< 未解码 */
    APP_CAN_DM_KIND_MIT,      /**< MIT 控制 */
    APP_CAN_DM_KIND_POS_VEL,  /**< 位置速度 */
    APP_CAN_DM_KIND_VEL,      /**< 速度 */
    APP_CAN_DM_KIND_FORCE_POS,/**< 力位混控 */
    APP_CAN_DM_KIND_ENABLE,   /**< 使能 */
    APP_CAN_DM_KIND_DISABLE,  /**< 失能 */
    APP_CAN_DM_KIND_CLEAR,    /**< 清故障 */
} app_can_dm_kind_t;

/**
 * @brief 解码结果（物理量，已按线缆定标还原）
 *
 * 单位：p_des_rad [rad]（输出轴）、v_des_rad_s [rad/s]（输出轴）、
 *       kp [Nm/rad]、kd [Nm/(rad/s)]、t_ff_nm [Nm]，均为用户坐标。
 */
typedef struct {
    app_can_dm_kind_t kind;   /**< 帧类别 */
    float p_des_rad;          /**< 目标位置（MIT/POS_VEL/FORCE_POS） */
    float v_des_rad_s;        /**< 目标速度（MIT/POS_VEL/VEL） */
    float kp;                 /**< MIT 刚度 [Nm/rad] */
    float kd;                 /**< MIT 阻尼 [Nm/(rad/s)] */
    float t_ff_nm;            /**< MIT 前馈力矩 [Nm] */
    float vel_limit_rad_s;    /**< 力位混控速度上限 [rad/s] */
    float iq_ratio;           /**< 力位混控电流标幺 [0,1]（乘 i_peak_10s_a 后为 A） */
} app_can_dm_frame_t;

/**
 * @brief 线缆定标集合（便于主机测试用不同量程）
 *
 * t_ff_max_nm 与 t_fb_max_nm 刻意分离：MIT 前馈保持 DM 的 ±10 Nm，
 * 反馈 T 用本电机全量程 ±200 Nm。这样 DM 主机的 MIT 控制输入仍兼容，
 * 只有读取反馈转矩的主机需要改常量。
 */
typedef struct {
    float p_max_rad;   /**< 位置半量程 [rad] */
    float v_max_rad_s; /**< 速度半量程 [rad/s] */
    float t_ff_max_nm; /**< MIT 前馈半量程 [Nm] */
    float t_fb_max_nm; /**< 反馈转矩半量程 [Nm] */
    float kp_max;      /**< Kp 满量程 [Nm/rad] */
    float kd_max;      /**< Kd 满量程 [Nm/(rad/s)] */
} app_can_dm_scale_t;

/** @brief 默认线缆定标（DM 手册值） */
const app_can_dm_scale_t* app_can_dm_default_scale(void);

/**
 * @brief 解析标准帧 ID
 * @param id 11 位标准帧 ID
 * @param type 输出：帧类型 1..15
 * @param node 输出：节点号 0..15
 * @return true = `(id >> 8) == 0x1` 且 type != 0
 */
bool app_can_dm_parse_id(uint32_t id, uint8_t* type, uint8_t* node);

/** @brief 构造控制帧 ID */
uint32_t app_can_dm_make_id(uint8_t type, uint8_t node);

/** @brief 反馈帧 ID = 0x180 | node */
uint32_t app_can_dm_feedback_id(uint8_t node);

/**
 * @brief 解码一条本机控制帧
 *
 * @param id 帧 ID
 * @param data 载荷
 * @param len DLC
 * @param node 本机节点号
 * @param scale 线缆定标（NULL = 默认）
 * @param out 输出
 * @return 0 = 成功；1 = 非本机/未知类型（不回帧）；-1 = 长度/magic/非有限值非法
 */
int app_can_dm_decode(uint32_t id, const uint8_t* data, uint8_t len, uint8_t node,
                      const app_can_dm_scale_t* scale, app_can_dm_frame_t* out);

/**
 * @brief 编码反馈帧
 *
 * D0=(ERR<<4)|node；POS 16 位；VEL 12 位；T 12 位；D6=T_MOS；D7=T_Rotor。
 * POS/VEL/T 按线缆定标截断（不四舍五入），越界饱和。
 * 反馈 T 使用 `scale->t_fb_max_nm`（默认 ±200 Nm），与 MIT t_ff 的 ±10 Nm 分离。
 *
 * @return 8（固定长度）；参数非法返回 0
 */
uint8_t app_can_dm_encode_feedback(uint8_t* out, uint8_t node, uint8_t err, float pos_rad,
                                   float vel_rad_s, float torque_nm, uint8_t t_mos_c,
                                   uint8_t t_rotor_c, const app_can_dm_scale_t* scale);

/**
 * @brief 浮点 → 定标无符号整数（截断；非有限值返回 0）
 */
uint32_t app_can_dm_float_to_uint(float x, float min, float max, uint32_t bits);

/** @brief 定标无符号整数 → 浮点 */
float app_can_dm_uint_to_float(uint32_t x, float min, float max, uint32_t bits);

#ifdef __cplusplus
}
#endif

#endif /* APP_CAN_PROTOCOL_H */
