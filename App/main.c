/**
 * @file    main.c
 * @brief   程序入口：板级初始化 + 应用主循环
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "board.h"

extern void app_init(void);
extern void app_run(void);

/**
 * @brief  程序入口：板级初始化后进入应用主循环。
 * @return 退出码（正常运行时不会返回）
 */
int main(void) {
    board_init();

    app_init();

    while (1) {
        app_run();
    }

    return 0;
}
