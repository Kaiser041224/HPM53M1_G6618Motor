#include "app_uart.h"

#include "intf_uart.h"

#include <string.h>

/* Driver registration */
extern void hpm_uart_driver_register(void);

/* 控制台设备对象（init 时解析并缓存，热路径无注册表查表） */
static const intf_uart_t *s_console;

int app_uart_init(void)
{
    intf_uart_cfg_t cfg = {
        .baudrate = 115200U,
        .data_bits = 8U,
        .stop_bits = 1U,
        .parity = 0U, /* 无校验 */
        .flow_ctrl = false,
    };

    hpm_uart_driver_register();

    s_console = intf_uart_get(APP_UART_PORT_CONSOLE);
    if (s_console == NULL) {
        return -1;
    }

    return s_console->init(&cfg);
}

int app_uart_write(const uint8_t *data, size_t len)
{
    if (s_console == NULL) {
        return -1;
    }
    return s_console->transmit(data, len, 100U);
}

int app_uart_write_str(const char *str)
{
    return app_uart_write((const uint8_t *) str, strlen(str));
}

int app_uart_read(uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (s_console == NULL) {
        return -1;
    }
    return s_console->receive(data, len, timeout_ms);
}
