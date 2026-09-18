#include "app_uart.h"

#include "intf_uart.h"

#include <string.h>

/* Driver registration */
extern void hpm_uart_driver_register(void);

void app_uart_init(void)
{
    hpm_uart_driver_register();

    intf_uart_cfg_t cfg = {
        .baudrate = 115200U,
        .data_bits = 8U,
        .stop_bits = 1U,
        .parity = 0U, /* 无校验 */
        .flow_ctrl = false,
    };

    (void) intf_uart_init(APP_UART_PORT_CONSOLE, &cfg);
}

int app_uart_write(const uint8_t *data, size_t len)
{
    return intf_uart_transmit(APP_UART_PORT_CONSOLE, data, len, 100U);
}

int app_uart_write_str(const char *str)
{
    return app_uart_write((const uint8_t *) str, strlen(str));
}
