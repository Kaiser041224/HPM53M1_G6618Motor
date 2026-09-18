#include "app_debug_uart.h"

#include "app_debug_rtt.h"
#include "app_uart.h"
#include "intf_clock.h"
#include "intf_uart.h"

#include <stdint.h>
#include <stdio.h>

#define UART_TEST_TX_PERIOD_MS (1000U)
#define UART_TEST_RX_BUF_SIZE  (64U)

static uint32_t s_tick;
static uint32_t s_rx_total;
static uint32_t s_last_tx_cycle;

void app_debug_uart_init(void)
{
    app_uart_init();

    app_debug_printf("[UART] self-test: port0 @115200 8N1 (PA00=TX, PA01=RX)\r\n");
    (void) app_uart_write_str("[UART] self-test start\r\n");
}

void app_debug_uart_run_once(void)
{
    uint8_t buf[UART_TEST_RX_BUF_SIZE];
    uint32_t now;
    uint32_t period_cycles;
    int n;

    /* 1) RX：排空环形缓冲 -> 原样回显 + RTT 记录 */
    n = intf_uart_receive(APP_UART_PORT_CONSOLE, buf, sizeof(buf), 0U);
    if (n > 0) {
        uint8_t last = buf[n - 1];
        char printable = ((last >= 0x20U) && (last < 0x7FU)) ? (char) last : '.';

        s_rx_total += (uint32_t) n;
        (void) intf_uart_transmit(APP_UART_PORT_CONSOLE, buf, (size_t) n, 100U);
        app_debug_printf("[UART] rx n=%d total=%u last=0x%02X ('%c')\r\n", n,
                         (unsigned) s_rx_total, (unsigned) last, printable);
    }

    /* 2) TX：每秒发送一行状态 */
    now = intf_clock_get_cycle();
    period_cycles = (intf_clock_get_cpu_freq() / 1000U) * UART_TEST_TX_PERIOD_MS;
    if ((uint32_t) (now - s_last_tx_cycle) >= period_cycles) {
        char line[96];
        int len;

        s_last_tx_cycle = now;
        s_tick++;
        len = snprintf(line, sizeof(line), "uart: tick=%u rx_total=%u\r\n", (unsigned) s_tick,
                       (unsigned) s_rx_total);
        if (len > 0) {
            (void) intf_uart_transmit(APP_UART_PORT_CONSOLE, (const uint8_t *) line, (size_t) len,
                                      100U);
        }
    }
}
