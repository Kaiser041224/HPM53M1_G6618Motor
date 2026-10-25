/**
 * @file    app_cogging_identify.h
 * @brief   齿槽/传动角表低速正反扫自整定
 * @author  Kaiser
 *
 * 标定边界：电机本体 + 减速器，不装外部负载。
 * 无参数时默认多点 1.0/2.0/3.0 rad/s，每点每方向 2 圈；
 * 指定 speed 时为单点，默认每方向 4 圈。结果不自动使能。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_COGGING_IDENTIFY_H
#define APP_COGGING_IDENTIFY_H

#include "app_cogging.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_COGGING_IDENTIFY_DEFAULT_SPEED_RAD_S (2.0f)
#define APP_COGGING_IDENTIFY_DEFAULT_REVS        (4U)
#define APP_COGGING_IDENTIFY_MULTI_REVS           (2U)
#define APP_COGGING_IDENTIFY_MAX_POINTS           (4U)

/** @brief 自整定状态 */
typedef enum {
    APP_COGGING_IDENTIFY_IDLE = 0,
    APP_COGGING_IDENTIFY_SETTLE_FWD,
    APP_COGGING_IDENTIFY_SWEEP_FWD,
    APP_COGGING_IDENTIFY_STOP_FWD,
    APP_COGGING_IDENTIFY_SETTLE_REV,
    APP_COGGING_IDENTIFY_SWEEP_REV,
    APP_COGGING_IDENTIFY_STOP_REV,
    APP_COGGING_IDENTIFY_FINALIZE,
    APP_COGGING_IDENTIFY_DONE,
    APP_COGGING_IDENTIFY_FAILED,
} app_cogging_identify_state_t;

/** @brief 失败原因 */
typedef enum {
    APP_COGGING_IDENTIFY_FAIL_NONE = 0,
    APP_COGGING_IDENTIFY_FAIL_STATE,
    APP_COGGING_IDENTIFY_FAIL_MOTION,
    APP_COGGING_IDENTIFY_FAIL_FEEDBACK,
    APP_COGGING_IDENTIFY_FAIL_TIMEOUT,
    APP_COGGING_IDENTIFY_FAIL_SAMPLE,
    APP_COGGING_IDENTIFY_FAIL_QUALITY,
    APP_COGGING_IDENTIFY_FAIL_ABORTED,
    APP_COGGING_IDENTIFY_FAIL_BUSY,
} app_cogging_identify_failure_t;

/** @brief 单个速度测试点的正/反向 q 电流统计（用于观测速度依赖） */
typedef struct {
    float speed_rad_s;
    uint16_t revs;
    uint32_t fwd_samples;
    uint32_t rev_samples;
    float fwd_iq_mean_a;
    float rev_iq_mean_a;
    float fwd_iq_rms_a;
    float rev_iq_rms_a;
} app_cogging_identify_point_t;

/** @brief 观测快照 */
typedef struct {
    app_cogging_identify_state_t state;
    app_cogging_identify_failure_t failure;
    bool busy;
    bool result_valid;
    bool rotor_valid;
    bool output_valid;
    float speed_cmd_rad_s;
    float speed_rad_s;
    float travel_rad;
    float progress;
    uint32_t elapsed_ms;
    uint32_t samples;
    uint16_t revs;
    uint8_t point_index;
    uint8_t point_count;
    app_cogging_identify_point_t points[APP_COGGING_IDENTIFY_MAX_POINTS];
    algo_cogging_stats_t rotor_stats;
    algo_cogging_stats_t output_stats;
} app_cogging_identify_snapshot_t;

void app_cogging_identify_init(void);
/**
 * @brief 启动标定
 * @param speed_rad_s 输出轴速度幅值 [rad/s]；<=0 使用默认多点 1.0/2.0/3.0 rad/s
 * @param revs 每个速度点每方向输出圈数（0 用默认值）
 * @return 0 = 启动；-1 参数/忙；-2 FOC 非 READY/RUN；-3 motion 未标定/活动；
 *         -4 其他辨识运行；-5 motion 命令失败
 */
int app_cogging_identify_start(float speed_rad_s, uint16_t revs);
void app_cogging_identify_abort(void);
void app_cogging_identify_run_once(void);
bool app_cogging_identify_busy(void);
void app_cogging_identify_get_snapshot(app_cogging_identify_snapshot_t* out);
/** @brief 读取最近结果（不改变使能） */
int app_cogging_identify_get_result(app_cogging_tables_t* out);
/** @brief FOC OFF 时把最近结果复制到活动表；不自动使能 */
int app_cogging_identify_apply(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_COGGING_IDENTIFY_H */
