/**
 * @file    app_debug_rtt.c
 * @brief   SEGGER RTT 调试输出封装
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_debug_rtt.h"

#include "SEGGER_RTT.h"

#include <stdarg.h>
#include <stdio.h>

int app_debug_printf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    SEGGER_RTT_WriteString(0, buf);
    return len;
}
