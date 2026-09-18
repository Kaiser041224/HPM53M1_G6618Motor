#ifndef APP_UART_H
#define APP_UART_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UART0：PA00=TXD / PA01=RXD（J10），上位机调参 + ISP */
#define APP_UART_PORT_CONSOLE (0U)

/**
 * @brief 注册 UART 驱动并初始化控制台端口（115200 8N1，中断 RX）。
 */
void app_uart_init(void);

/**
 * @brief 阻塞发送（100ms 超时）。
 * @return 0 成功，-1 失败
 */
int app_uart_write(const uint8_t *data, size_t len);

/**
 * @brief 发送以 '\0' 结尾的字符串。
 * @return 0 成功，-1 失败
 */
int app_uart_write_str(const char *str);

#ifdef __cplusplus
}
#endif

#endif /* APP_UART_H */
