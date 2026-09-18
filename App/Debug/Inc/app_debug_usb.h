#ifndef APP_DEBUG_USB_H
#define APP_DEBUG_USB_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief USB 自检初始化：注册驱动并初始化 USB0 为 CDC 虚拟串口。
 */
void app_debug_usb_init(void);

/**
 * @brief USB 自检周期任务：
 *        1) 上报主机端口打开/关闭（DTR 变化）
 *        2) 排空 RX 缓冲并回显（验证 RX 通路，回显内容同时打印到 RTT）
 *        3) 端口打开后每秒发送一行状态（验证 TX 通路）
 */
void app_debug_usb_run_once(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_DEBUG_USB_H */
