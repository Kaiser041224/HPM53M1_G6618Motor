/**
 * @file    test_util.h
 * @brief   状态 LED 灯语主机自测断言宏
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef TEST_UTIL_H
#define TEST_UTIL_H

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

#endif /* TEST_UTIL_H */
