/** @file app_motion.h
 * @brief 游标轴端反馈、力矩/速度控制及安装标定
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef APP_MOTION_H
#define APP_MOTION_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** @brief 实际轴端单圈角度，已应用机械零点；只供调试，不作为控制输入 */
extern volatile float g_motion_shaft_angle_rad;
/** @brief 实际轴端单圈角度[0,360)，不是游标磁铁角度 */
extern volatile float g_motion_shaft_angle_deg;
/** @brief 最近一次反馈有效=1；无效时角度保持最后有效值 */
extern volatile uint32_t g_motion_shaft_angle_valid;
/** @brief 最近一次拒绝/输出撤销原因，不自动恢复 */
typedef enum {
    APP_MOTION_REASON_NONE = 0,
    APP_MOTION_REASON_SAMPLE,
    APP_MOTION_REASON_DUPLICATE,
    APP_MOTION_REASON_VERNIER,
    APP_MOTION_REASON_TIMING,
    APP_MOTION_REASON_PARAMETER,
    APP_MOTION_REASON_FOC_STATE,
    APP_MOTION_REASON_SUBMIT,
    APP_MOTION_REASON_USER_STOP,
    APP_MOTION_REASON_CALIBRATION,
} app_motion_reason_t;
typedef struct {
    float shaft_rad;                 /**< 已应用机械零点的轴端单圈角[rad] */
    float speed_rad_s;               /**< 轴端滤波速度[rad/s]，非反馈限速 */
    float torque_nm;                 /**< motion模型转矩请求[Nm]；不是实测轴端转矩 */
    float iq_a;                      /**< motion电流请求[A]，不是FOC实测电流 */
    float iq_accepted_a;             /**< FOC最近确认执行的接受iq[A]，诊断非实测 */
    float phase_turns;               /**< 已应用游标安装相位[turn] */
    uint32_t good_count, bad_count, calibration_samples;
    uint32_t sample_failure_count, duplicate_count, vernier_failure_count;
    uint32_t vernier_fallback_count; /**< 游标失配兜底测速样本数（转子角+最近圈号） */
    uint32_t timing_failure_count;
    uint32_t hold_count;        /**< 坏拍有界保持事件累计（active期间质量类坏拍进入保持的次数） */
    app_motion_reason_t reason; /**< 最近拒绝/输出撤销；成功新命令清零 */
    bool current_limited, voltage_saturated; /**< 最近FOC执行限幅状态（仅属本请求时有效） */
    bool calibrated, zero_valid, feedback_valid, active, calibrating, calibration_done;
} app_motion_snapshot_t;
/** @brief 初始化/恢复参数；采样计时回调停止期间调用，不使能 */
void app_motion_init(void);
/** @brief 10kHz入口：读ADC发布的双编码器配对快照，再估计与控制 */
void app_motion_step(void);
/** @brief 静止OFF采1000组相位，期间不可移动 */
int app_motion_calibrate(void);
/** @brief OFF应用最近静止标定，旧机械零点失效 */
int app_motion_apply(void);
/** @brief OFF当前机械坐标置零 */
int app_motion_zero(void);
/** @brief OFF持久化相位/零点 */
int app_motion_save(void);
/** @brief 释放motion输出（零力矩请求并退出驱动）；不改变FOC状态/桥 */
int app_motion_release_output(void);
/** @brief 配置速度增益kp/ki与摩擦前馈Tc/Kv（轴端），不驱动时可改，写透参数单例 */
int app_motion_gains(float kp, float ki, float coulomb_nm, float viscous_nm_per_rad_s);
/** @brief 轴端模型转矩Nm，电流硬限制1A */
int app_motion_torque(float torque);
/** @brief 轴端rad/s目标；命令不限幅，输出受电流硬限制 */
int app_motion_speed(float speed);
/** @brief 辨识期挂起/恢复摩擦前馈（速度模式FF置零，PI全权；不改参数） */
void app_motion_friction_suspend(bool suspend);
/** @brief 撤销motion输出，随后FOC正常缓降；返回FOC接受状态，0成功 */
int app_motion_stop(void);
/** @brief motion命令返回码（0成功；负值为拒绝分类，终端据此给处置提示） */
typedef enum {
    APP_MOTION_CMD_OK = 0,
    APP_MOTION_CMD_BAD_PARAM = -1,      /**< 非有限值或Kt非法 */
    APP_MOTION_CMD_NOT_CALIBRATED = -2, /**< 未标定/标定中/方向基准不符 */
    APP_MOTION_CMD_STALE_FEEDBACK = -3, /**< 反馈失效或超过500us */
    APP_MOTION_CMD_TARGET_LIMIT = -4,   /**< 目标幅值超限（力矩≤50Kt；速度命令不限幅） */
    APP_MOTION_CMD_GAIN_NOT_SET = -5,   /**< 速度模式未配置增益 */
    APP_MOTION_CMD_FOC_NOT_READY = -6,  /**< FOC非READY/RUN（off/stopping/CALIB） */
    APP_MOTION_CMD_BASE_CHANGED = -7,   /**< 活动中Kt/方向基准被改 */
    APP_MOTION_CMD_MODE_LOCKED = -8,    /**< 活动中不得切换力矩/速度 */
    APP_MOTION_CMD_OWNER_DENIED = -9,   /**< 生产者授权被拒（校准/vtest占用） */
} app_motion_cmd_status_t;
/** @brief 观测一致副本 */
void app_motion_get_snapshot(app_motion_snapshot_t* out);
#ifdef __cplusplus
}
#endif
#endif
