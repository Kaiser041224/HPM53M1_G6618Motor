/*
 * USB CDC 自检（USB0 虚拟串口）
 *
 * 测试内容：
 *   1) 主机端口打开/关闭检测（DTR）
 *   2) RX 回显：上位机发送的数据原样回发（对应 UART 自检）
 *   3) 端口打开后每秒发送一行状态（TX 通路）
 *
 * 硬件：USB0 专用引脚 USB_DP/USB_DM（pin48/49）→ J10（U12 ESD + L5 共模电感）。
 * 说明：板卡由外部电源供电，USB 仅用于数据（连接器无 VBUS）。
 */

#include "app_debug_usb.h"

#include "app_debug_rtt.h"
#include "app_usb.h"
#include "intf_clock.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define USB_TEST_TX_PERIOD_MS (1000U)
#define USB_TEST_RX_BUF_SIZE  (128U)

static uint32_t s_tick;
static uint32_t s_rx_total;
static uint32_t s_last_tx_cycle;
static bool s_last_dtr;
static bool s_banner_sent;

void app_debug_usb_init(void)
{
    app_usb_init();

    s_banner_sent = false;
    app_debug_printf("[USB] self-test: USB0 CDC ACM (J10 D+/D-), HS\r\n");
}

void app_debug_usb_run_once(void)
{
    uint8_t buf[USB_TEST_RX_BUF_SIZE];
    uint32_t now = intf_clock_get_cycle();
    uint32_t period_cycles = (intf_clock_get_cpu_freq() / 1000U) * USB_TEST_TX_PERIOD_MS;
    bool dtr = app_usb_is_dtr();
    int n;

    /* 1) 主机 DTR 变化上报（信息性；部分串口工具默认不置 DTR） */
    if (dtr != s_last_dtr) {
        app_debug_printf("[USB] host DTR %s (DTR=%d)\r\n", dtr ? "ON" : "OFF", dtr ? 1 : 0);
        s_last_dtr = dtr;
    }

    /* 2) 启动 banner：非阻塞写入（0 超时），成功入队即止。
       主机未打开端口时不会阻塞控制环（曾导致 25kHz 掉到 9Hz）。 */
    if (!s_banner_sent) {
        const char *banner = "[USB] self-test start\r\n";

        if (app_usb_write_timeout((const uint8_t *) banner, strlen(banner), 0U) == 0) {
            s_banner_sent = true;
        }
    }

    /* 3) RX：排空环形缓冲 -> 原样回显 + RTT 记录 */
    n = app_usb_read(buf, sizeof(buf));
    if (n > 0) {
        uint8_t last = buf[n - 1];
        char printable = ((last >= 0x20U) && (last < 0x7FU)) ? (char) last : '.';

        s_rx_total += (uint32_t) n;
        (void) app_usb_write(buf, (size_t) n);
        app_debug_printf("[USB] rx n=%d total=%u last=0x%02X ('%c')\r\n", n, (unsigned) s_rx_total,
                         (unsigned) last, printable);
    }

    /* 4) TX：每秒发送一行状态（不依赖 DTR；未枚举/未打开端口时写失败静默） */
    if ((uint32_t) (now - s_last_tx_cycle) >= period_cycles) {
        char line[96];
        int len;

        s_last_tx_cycle = now;
        s_tick++;
        len = snprintf(line, sizeof(line), "usb: tick=%u rx_total=%u\r\n", (unsigned) s_tick,
                       (unsigned) s_rx_total);
        if (len > 0) {
            /* 非阻塞：主机未打开端口/端点忙时直接跳过，不阻塞控制环 */
            (void) app_usb_write_timeout((const uint8_t *) line, (size_t) len, 0U);
        }
    }
}
