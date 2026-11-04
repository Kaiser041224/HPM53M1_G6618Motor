/**
 * @file    test_main.c
 * @brief   FOC 纯数学层主机自测入口
 * @author  Kaiser
 *
 * 约定（重要）：每新增一个 test_*.c，必须在此追加：
 *   1) 文件顶部的 `void test_xxx(void);` 声明；
 *   2) main() 内的 `test_xxx();` 调用。
 *   run.sh 不自动发现测试函数；漏接线会导致"假绿"。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

int g_checks = 0;
int g_fails = 0;

void test_foc_math(void);
void test_foc_angle(void);
void test_foc_modulation(void);
void test_foc_current(void);
void test_id_encoder(void);

int main(void) {
    printf("FOC host tests\n");
    test_foc_math();
    test_foc_angle();
    test_foc_modulation();
    test_foc_current();
    test_id_encoder();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    if (g_checks == 0) {
        printf("ERROR: no checks executed (test wiring missing?)\n");
        return 1;
    }
    return (g_fails == 0) ? 0 : 1;
}
