#ifndef APP_DEBUG_UART_H
#define APP_DEBUG_UART_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief UART 自检初始化：注册驱动并初始化 UART0（115200 8N1）。
 */
void app_debug_uart_init(void);

/**
 * @brief UART 自检周期任务：
 *        1) 排空 RX 缓冲并回显（验证 RX 通路，回显内容同时打印到 RTT）
 *        2) 每秒通过 UART0 发送一行状态（验证 TX 通路）
 */
void app_debug_uart_run_once(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_DEBUG_UART_H */
