/**
 * @file    app_terminal_job.h
 * @brief   Terminal 长命令 job 框架（单前台 job：1kHz tick + 中止）
 * @author  Kaiser
 *
 * 用途：Terminal 命令本体必须快速返回（运行在 1kHz 慢任务，不得阻塞控制环）；
 *       monitor、标定、整定等长流程注册为 job，由 1kHz tick 驱动。
 *
 * 中止触发源：Ctrl-C（用户回调）/ 任意新输入（sget）/ 新 job 启动。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_TERMINAL_JOB_H
#define APP_TERMINAL_JOB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief job 描述（调用方提供静态生命周期实例）
 */
typedef struct {
    const char *name;              /**< job 名（提示输出用） */
    void (*tick)(uint32_t now_ms); /**< 1kHz 周期回调（不可为 NULL） */
    void (*abort)(void);           /**< 中止回调（可为 NULL） */
    bool active;                   /**< 运行标志（框架维护） */
} app_terminal_job_t;

/**
 * @brief 启动 job（已有 job 先中止）。
 * @param job job 描述（静态生命周期）
 * @return 0 = 成功；-1 = 参数非法
 */
int app_terminal_job_start(app_terminal_job_t *job);

/**
 * @brief 1kHz 周期驱动（无 job 时空操作）。
 * @param now_ms 系统毫秒计数
 */
void app_terminal_job_tick(uint32_t now_ms);

/**
 * @brief 中止当前 job（无 job 时空操作）。
 */
void app_terminal_job_abort(void);

/**
 * @brief 是否有 job 运行。
 * @return true = 有
 */
bool app_terminal_job_is_active(void);

/**
 * @brief 当前 job 名。
 * @return 名称；无 job 时返回 NULL
 */
const char *app_terminal_job_name(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_TERMINAL_JOB_H */
