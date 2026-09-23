/**
 * @file    main.c
 * @brief   程序入口：板级初始化 + FreeRTOS 调度（bring-up 超循环任务化）
 * @author  Kaiser
 *
 * 调度结构（设计文档：docs/superpowers/specs/2026-09-23-rtos-foundation-design.md）：
 *   main → board_init → 创建 app 任务 → vTaskStartScheduler
 *   app 任务 = app_init + tick 自检 + app_run（原样超循环，零改动）
 *
 * 说明：app_init 必须在调度器启动后执行（电流零标定等依赖 ISR；
 *       CONFIG_DISABLE_GLOBAL_IRQ_ON_STARTUP=1 使全局中断由调度器开启）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "board.h"

#include "app_rtos.h"

#include "FreeRTOS.h"
#include "task.h"

extern void app_init(void);
extern void app_run(void);

/**
 * @brief bring-up 任务体：既有自检 + 超循环原样运行。
 * @param arg 未使用
 */
static void app_bringup_task(void* arg) {
    (void)arg;

    app_init();
    app_rtos_selfcheck_tick();
    app_run(); /* 内含 for(;;)，永不返回 */
}

/**
 * @brief  程序入口：板级初始化后进入 FreeRTOS 调度。
 * @return 退出码（正常运行时不会返回）
 */
int main(void) {
    board_init();

    if (xTaskCreate(app_bringup_task, "app", APP_RTOS_STACK_BRINGUP_WORDS, NULL,
                    APP_RTOS_PRIO_BRINGUP, NULL) != pdPASS) {
        app_rtos_fatal(__FILE__, __LINE__);
    }

    vTaskStartScheduler();

    /* 调度器不应返回 */
    app_rtos_fatal(__FILE__, __LINE__);
    return 0;
}
