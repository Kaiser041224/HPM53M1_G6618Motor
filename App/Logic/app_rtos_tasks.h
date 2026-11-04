/**
 * @file    app_rtos_tasks.h
 * @brief   应用任务编排接口（FOC 快车道 / IO / 诊断三任务）
 * @author  Kaiser
 *
 * 设计文档：docs/superpowers/specs/2026-09-23-foc-fastlane-task-structure-design.md
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_RTOS_TASKS_H
#define APP_RTOS_TASKS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 应用启动：创建 rtt_log 与 app_io 任务并启动调度器（不返回）。
 *
 * app_io 任务在调度器内执行 app_init()（电流零点标定依赖 ISR）后创建
 * app_fast / app_diag 任务，再进入 1ms IO 循环。
 */
void app_application_start(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_RTOS_TASKS_H */
