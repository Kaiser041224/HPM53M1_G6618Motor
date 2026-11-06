/**
 * @file    test_main.c
 * @brief   USB 协议服务主机自测入口
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

int g_checks = 0;
int g_fails = 0;

void test_usb_proto(void);

int main(void) {
    test_usb_proto();

    printf("%d checks, %d failures\n", g_checks, g_fails);
    if (g_checks == 0) {
        printf("ERROR: no checks executed (test wiring missing?)\n");
        return 1;
    }
    return (g_fails == 0) ? 0 : 1;
}
