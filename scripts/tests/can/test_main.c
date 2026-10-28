/** @file test_main.c
 * @brief CAN 协议编解码主机自测入口
 * @author Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "test_util.h"

int g_checks = 0;
int g_fails = 0;

void test_can_id(void);
void test_can_decode_control(void);
void test_can_encode_status(void);
void test_can_mit_compact(void);
void test_can_mit_dual(void);

int main(void) {
    test_can_id();
    test_can_decode_control();
    test_can_encode_status();
    test_can_mit_compact();
    test_can_mit_dual();

    printf("CAN host tests\n%d checks, %d failures\n", g_checks, g_fails);
    return (g_fails == 0) ? 0 : 1;
}
