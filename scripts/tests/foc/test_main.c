/**
 * @file    test_main.c
 * @brief   FOC 纯数学层主机自测入口
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

int g_checks = 0;
int g_fails = 0;

int main(void) {
    printf("FOC host tests\n");
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return (g_fails == 0) ? 0 : 1;
}
