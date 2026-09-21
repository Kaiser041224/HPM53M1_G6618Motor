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
#include <string.h>

/** 旁路写入器（NULL = 仅 RTT） */
static app_debug_writer_t s_writer;

void app_debug_set_writer(app_debug_writer_t writer) {
    s_writer = writer;
}

int app_debug_printf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    SEGGER_RTT_WriteString(0, buf);

    if (s_writer != NULL) {
        s_writer(buf, strlen(buf));
    }

    return len;
}
