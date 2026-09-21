/**
 * @file    test_util.h
 * @brief   FOC 主机自测断言宏（宿主机编译，不参与目标固件）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <math.h>
#include <stdbool.h>
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
        g_checks++;                                                                                \
        float _d = fabsf((float)(actual) - (float)(expected));                                     \
        if (!(_d <= (float)(tol))) {                                                               \
            g_fails++;                                                                             \
            printf("FAIL %s:%d: |%s - %s| = %g > %g\n", __FILE__, __LINE__, #actual, #expected,    \
                   (double)_d, (double)(tol));                                                     \
        }                                                                                          \
    } while (0)

#endif /* TEST_UTIL_H */
