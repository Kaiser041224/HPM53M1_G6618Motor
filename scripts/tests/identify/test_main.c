/**
 * @file    test_main.c
 * @brief   电角度自校准编排主机自测入口
 * @author  Kaiser
 *
 * 约定：每新增 test_*.c 必须在 main() 内接线，run.sh 不自动发现函数。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

int g_checks = 0;
int g_fails = 0;

void test_app_motor_identify(void);

int main(void) {
    printf("Identify host tests\n");
    test_app_motor_identify();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    if (g_checks == 0) {
        printf("ERROR: no checks executed (test wiring missing?)\n");
        return 1;
    }
    return (g_fails == 0) ? 0 : 1;
}
