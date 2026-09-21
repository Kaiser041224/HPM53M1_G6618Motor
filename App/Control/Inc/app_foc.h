/**
 * @file    app_foc.h
 * @brief   FOC 编排：状态机 / 模式 / 联锁 / 25kHz 入口
 * @author  Kaiser
 *
 * 状态机（spec §4.1）：
 *   OFF → (enable) → READY → (首次非零给定) → RUN
 *   READY/RUN → (cal 请求) → CALIB → (完成/中止) → READY
 *   任意 → (fault != NORMAL) → FAULT（零矢量 + 关桥）→ (disable) → OFF
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_FOC_H
#define APP_FOC_H

#include <stdbool.h>
#include <stdint.h>

#include "app_foc_current.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FOC 状态
 */
typedef enum {
    APP_FOC_STATE_OFF = 0, /**< 输出关闭 */
    APP_FOC_STATE_READY,   /**< 桥已使能，零矢量 */
    APP_FOC_STATE_RUN,     /**< 电流闭环 */
    APP_FOC_STATE_CALIB,   /**< 辨识模式（强制角 + 辨识模块驱动） */
    APP_FOC_STATE_FAULT,   /**< 故障门控（零矢量 + 关桥） */
} app_foc_state_t;

/**
 * @brief 角度来源
 */
typedef enum {
    APP_FOC_ANGLE_ENCODER = 0, /**< 编码器（正常闭环） */
    APP_FOC_ANGLE_FORCED,      /**< 强制角（辨识/调试） */
} app_foc_angle_source_t;

/**
 * @brief 初始化（上电状态 OFF）
 */
void app_foc_init(void);

/**
 * @brief 25kHz 节拍（app_logic 主循环调用）
 */
void app_foc_run_once(void);

/**
 * @brief 使能（OFF → READY）：检查故障/ADC/编码器/参数 → 使能逆变桥
 * @return 0 = 成功；-1 = 拒绝
 */
int app_foc_enable(void);

/**
 * @brief 关闭（任意 → OFF）：零矢量 + 关桥
 */
void app_foc_disable(void);

/**
 * @brief 设置转矩给定（限幅 ±i_q_max）
 * @return 0 = 成功；-1 = 状态不允许/参数非法
 */
int app_foc_set_iq_ref(float i_q_a);

/**
 * @brief 设置 d 轴给定（辨识/调试；默认 0）
 * @param i_d_a d 轴给定 [A]（不做单独限幅：内部电流矢量限幅兜底）
 * @return 0 = 成功；-1 = 状态不允许/参数非法
 */
int app_foc_set_id_ref(float i_d_a);

/**
 * @brief 设置角度来源与强制角
 * @param src 角度来源（编码器 / 强制）
 * @param theta_e_rad 强制电角度 [rad]（src = ENCODER 时忽略）
 * @return 0 = 成功；-1 = 枚举非法/非有限值/状态不允许
 */
int app_foc_set_angle_source(app_foc_angle_source_t src, float theta_e_rad);

/**
 * @brief 当前状态
 * @return 状态枚举
 */
app_foc_state_t app_foc_get_state(void);

/**
 * @brief 是否活动（!= OFF；供 Debug/Comm 互斥判断）
 * @return true = 活动
 */
bool app_foc_is_active(void);

/**
 * @brief 读取电流环快照
 * @param out 输出快照（不可为 NULL）
 */
void app_foc_get_snapshot(app_foc_current_snapshot_t* out);

/**
 * @brief 进入辨识模式（READY/RUN → CALIB）
 * @return 0 = 成功；-1 = 状态不允许
 */
int app_foc_enter_calib(void);

/**
 * @brief 退出辨识模式（CALIB → READY；恢复编码器角源；给定归零）
 */
void app_foc_exit_calib(void);

/**
 * @brief 辨识模式每拍更新激励（app_motor_identify 调用）
 * @param theta_e_rad 强制电角度 [rad]
 * @param i_d_ref d 轴电流给定 [A]
 * @param i_q_ref q 轴电流给定 [A]
 */
void app_foc_calib_set_excitation(float theta_e_rad, float i_d_ref, float i_q_ref);

/**
 * @brief 应用辨识出的电角度零点/方向（更新运行期角度链；不写参数）
 * @param offset_rad 电角度零点 [rad]
 * @param direction 方向（+1.0 / −1.0）
 */
void app_foc_apply_encoder_offset(float offset_rad, float direction);

/**
 * @brief 是否处于 FAULT 门控
 */
bool app_foc_fault_gate(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_FOC_H */
