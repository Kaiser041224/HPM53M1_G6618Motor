/**
 * @file    test_util.h
 * @brief   FOC 主机自测断言宏（宿主机编译，不参与目标固件）
 * @author  Kaiser
 *
 * 约定（重要）：
 *   新增 test_*.c 后，必须在 test_main.c 中追加其声明与调用；
 *   run.sh 只负责编译与运行，不会自动发现未接线的测试文件。
 *   断言按 float 域设计（FOC 全链路 float）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <math.h>
#include <stdio.h>

extern int g_checks;
extern int g_fails;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_fails++;                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
        }                                                                                          \
    } while (0)

#define CHECK_NEAR(actual, expected, tol)                                                          \
    do {                                                                                           \
        float delta = fabsf((float)(actual) - (float)(expected));                                  \
        float limit = (float)(tol);                                                                \
        g_checks++;                                                                                \
        if (!(delta <= limit)) {                                                                   \
            g_fails++;                                                                             \
            printf("FAIL %s:%d: |%s - %s| = %g > %g\n", __FILE__, __LINE__, #actual, #expected,    \
                   (double)delta, (double)limit);                                                  \
        }                                                                                          \
    } while (0)

#endif /* TEST_UTIL_H */
