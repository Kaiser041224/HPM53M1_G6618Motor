/**
 * @file    app_debug_uart.c
 * @brief   UART0 自检（RX 回显 / TX 状态行）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_debug_uart.h"

#include "app_debug_cmd.h"
#include "app_debug_rtt.h"
#include "app_uart.h"
#include "intf_clock.h"

#include <stdint.h>
#include <stdio.h>

/* 1Hz 状态行在 115200 下是一条约 40B 的阻塞写（≈3ms），会把同任务内的
 * 1kHz 遥测采样拉出一个 3ms 空洞（台架 dt max 3190µs 即由此产生）。
 * 默认关闭；需要心跳时置 1，并接受该抖动（或改到低优先级任务/非阻塞 TX）。 */
#ifndef UART_TEST_TX_HEARTBEAT
#define UART_TEST_TX_HEARTBEAT (0)
#endif

#define UART_TEST_TX_PERIOD_MS (1000U)
#define UART_TEST_RX_BUF_SIZE  (64U)

static uint32_t s_rx_total;

#if UART_TEST_TX_HEARTBEAT
static uint32_t s_tick;
static uint32_t s_last_tx_cycle;
#endif

void app_debug_uart_init(void) {
    int ret;

    ret = app_uart_init();
    app_debug_printf("[UART] self-test: port0 @115200 8N1 (PA00=TX, PA01=RX)\r\n");
    app_debug_printf("[UART] init: %s\r\n", (ret == 0) ? "OK" : "FAILED");

    ret = app_uart_write_str("[UART] self-test start\r\n");
    app_debug_printf("[UART] tx self-test: ret=%d\r\n", ret);
}

void app_debug_uart_run_once(void) {
    uint8_t buf[UART_TEST_RX_BUF_SIZE];
    int rx_len;

    /* 1) RX：排空环形缓冲 -> 原样回显 + RTT 记录 */
    rx_len = app_uart_read(buf, sizeof(buf), 0U);
    if (rx_len > 0) {
        uint8_t last = buf[rx_len - 1];
        char printable = ((last >= 0x20U) && (last < 0x7FU)) ? (char)last : '.';

        s_rx_total += (uint32_t)rx_len;
        (void)app_uart_write(buf, (size_t)rx_len);
        app_debug_printf(
            "[UART] rx n=%d total=%u last=0x%02X ('%c')\r\n", rx_len, (unsigned)s_rx_total,
            (unsigned)last, printable);
        app_debug_cmd_handle(buf, (size_t)rx_len);
    }

#if UART_TEST_TX_HEARTBEAT
    /* 2) TX：每秒发送一行状态（默认关闭，见文件头说明） */
    {
        uint32_t now = intf_clock_get_cycle();
        uint32_t period_cycles = (intf_clock_get_cpu_freq() / 1000U) * UART_TEST_TX_PERIOD_MS;

        if ((uint32_t)(now - s_last_tx_cycle) >= period_cycles) {
            char line[96];
            int len;

            s_last_tx_cycle = now;
            s_tick++;
            len = snprintf(
                line, sizeof(line), "uart: tick=%u rx_total=%u\r\n", (unsigned)s_tick,
                (unsigned)s_rx_total);
            if (len > 0) {
                if (app_uart_write((const uint8_t*)line, (size_t)len) != 0) {
                    app_debug_printf("[UART] tx FAILED (tick=%u)\r\n", (unsigned)s_tick);
                }
            }
        }
    }
#endif
}
