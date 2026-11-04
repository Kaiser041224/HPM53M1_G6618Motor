/**
 * @file    app_can_policy.h
 * @brief   DM CAN 帧 → 动作规划（纯函数，供主机回归与台架一致）
 * @author  Kaiser
 *
 * 把"协议帧"与"设备状态"映射为动作，不执行任何副作用。服务层负责执行与计数。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_CAN_POLICY_H
#define APP_CAN_POLICY_H

#include "app_can_protocol.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 服务层动作 */
typedef enum {
    APP_CAN_DM_ACT_DROP = 0,     /**< 丢弃（不执行） */
    APP_CAN_DM_ACT_ENABLE,       /**< app_foc_enable() */
    APP_CAN_DM_ACT_DISABLE,      /**< app_motion_stop()（受控软停） */
    APP_CAN_DM_ACT_CLEAR,        /**< app_fault_clear()（仅 OFF） */
    APP_CAN_DM_ACT_MIT,          /**< app_motion_mit() */
    APP_CAN_DM_ACT_POSITION,     /**< app_motion_position_limited()（位置速度模式） */
    APP_CAN_DM_ACT_VELOCITY,     /**< app_motion_speed() */
    APP_CAN_DM_ACT_FORCE_POS,    /**< app_motion_position_limited()（力位混控） */
    APP_CAN_DM_ACT_RELEASE_OUTPUT, /**< app_motion_release_output()，本帧不执行 */
} app_can_dm_action_t;

/** @brief FOC 状态数值（与 app_foc_state_t 对齐，避免策略层依赖 Control） */
#define APP_CAN_DM_FOC_OFF   (0U)
#define APP_CAN_DM_FOC_READY (1U)
#define APP_CAN_DM_FOC_RUN   (2U)
#define APP_CAN_DM_FOC_CALIB (3U)
#define APP_CAN_DM_FOC_FAULT (4U)

/** @brief 运动模式数值（与 app_motion_mode_t 对齐；0xFF = 未激活） */
#define APP_CAN_DM_MOTION_NONE     (0xFFU)
#define APP_CAN_DM_MOTION_TORQUE   (0U)
#define APP_CAN_DM_MOTION_SPEED    (1U)
#define APP_CAN_DM_MOTION_POSITION (2U)
#define APP_CAN_DM_MOTION_MIT      (3U)

/**
 * @brief 规划一条已通过校验的帧
 *
 * 规则（spec §3/§4/§6）：
 *   - 使能/失能/清故障：总是交服务层执行（前置条件由 FOC/故障模块判定）。
 *   - 控制帧：FOC 非 READY/RUN → 丢弃；MIT 未启用 → 丢弃。
 *   - 运动进行中收到不同**运动模式**的帧：释放输出并丢弃本帧，下一拍接受。
 *     POS_VEL 与 FORCE_POS 同属位置模式，不触发释放。
 *
 * @param frame 解码结果
 * @param active_mode 当前运动模式（APP_CAN_DM_MOTION_*）
 * @param active 运动是否激活
 * @param foc_state APP_CAN_DM_FOC_*
 * @param mit_enabled software.control.mit.enable
 * @param out 输出动作
 * @return 0 = 成功；-1 = 参数非法
 */
int app_can_dm_plan(const app_can_dm_frame_t* frame, uint8_t active_mode, bool active,
                    uint8_t foc_state, bool mit_enabled, app_can_dm_action_t* out);

/**
 * @brief 故障位图 → DM ERR 码
 *
 * 优先级：A(过流) > 8(过压) > 9(欠压) > D(通信) > F(其他/反馈无效) > 1/0。
 *
 * @param latched app_fault_get_latched() 位图
 * @param feedback_valid 反馈（位置/速度）是否可信
 * @param enabled FOC 是否 READY/RUN
 * @return DM ERR 码（0/1/8/9/A/D/F）
 */
uint8_t app_can_dm_err_from_fault(uint32_t latched, bool feedback_valid, bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* APP_CAN_POLICY_H */
