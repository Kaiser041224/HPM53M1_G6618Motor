/**
 * @file    app_motor_identify.h
 * @brief   电机辨识编排（V1：电角度辨识；job 驱动，非阻塞）
 * @author  Kaiser
 *
 * 职责：
 *   - 前置检查与安全警告
 *   - 25kHz：驱动 id_encoder 纯状态机，回喂测量、下发激励（经 app_foc）
 *   - 1kHz：进度/中止；辨识完成后做闭环验证并落库（RAM）
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_MOTOR_IDENTIFY_H
#define APP_MOTOR_IDENTIFY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 失败原因
 */
typedef enum {
    APP_IDENTIFY_REASON_NONE = 0,   /**< 无失败 */
    APP_IDENTIFY_REASON_TIMEOUT,    /**< 超时（辨识或编排） */
    APP_IDENTIFY_REASON_DIR,        /**< 方向判定无效（转子未跟随） */
    APP_IDENTIFY_REASON_QUALITY,    /**< 质量不足 */
    APP_IDENTIFY_REASON_RATIO,      /**< 极对数/传动比校验失败 */
    APP_IDENTIFY_REASON_NONFINITE,  /**< 非有限测量样本过多 */
    APP_IDENTIFY_REASON_VERIFY,     /**< 闭环验证失败 */
    APP_IDENTIFY_REASON_ENCODER,    /**< 编码器读失败/错误计数增长 */
    APP_IDENTIFY_REASON_FAULT,      /**< 故障 */
    APP_IDENTIFY_REASON_STATE,      /**< FOC 状态异常退出（非 CALIB） */
} app_identify_fail_t;

/**
 * @brief 辨识结果与状态（供 Comm/Debug 层读取并显示；Control 不直接打印）
 */
typedef struct {
    bool active;               /**< 流程进行中 */
    bool done;                 /**< 完成且验证通过 */
    bool failed;               /**< 失败（原因见 fail_reason） */
    float offset_rad;          /**< 电角度零点 [rad] */
    float direction;           /**< 方向（+1.0 / −1.0） */
    float quality;             /**< 质量 |Σ|/N */
    float ratio_err;           /**< 极对数校验偏差（相对） */
    float verify_mean_deg;     /**< 验证静默段角度残差均值 [deg]（信息量） */
    float verify_max_deg;      /**< 验证静默段角度残差峰值 [deg]（信息量） */
    float probe_travel_rad;    /**< 探针段机械行程 [rad]（判据：×direction ≥ 0.05） */
    float verify_drift_deg_s;  /**< 静默段残差漂移 [deg/s]（≈0 静止；大 = 转子被恒转矩驱动） */
    float progress;            /**< 进行中进度 0~1（RUN 阶段；非活动时为 0/1） */
    app_identify_fail_t fail_reason; /**< 失败原因 */
} app_motor_identify_result_t;

/**
 * @brief 是否处于辨识流程（进行中；完成/失败/中置后为 false）
 */
bool app_motor_identify_is_active(void);

/**
 * @brief 读取辨识结果/状态快照（Comm/Debug 显示用）
 * @param out 输出结果
 */
void app_motor_identify_get_result(app_motor_identify_result_t* out);

/**
 * @brief 25kHz：驱动辨识状态机并下发激励（由 app_foc_run_once 调用）
 * @return true = 已下发激励（app_foc 保持 CALIB 角源）
 */
bool app_motor_identify_fast_step(void);

/**
 * @brief 1kHz：进度显示 / 阶段推进（验证与落库）
 * @param now_ms 系统毫秒计数
 */
void app_motor_identify_run_once(uint32_t now_ms);

/**
 * @brief 启动辨识（Terminal `cal encoder`）：前置检查 + 进入 FOC CALIB
 * @return 0 = 成功；-1 = 拒绝（打印原因）
 */
int app_motor_identify_start(void);

/**
 * @brief 中止辨识（job abort / 故障 / 用户取消）：停止激励、恢复 FOC
 */
void app_motor_identify_abort(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_MOTOR_IDENTIFY_H */
