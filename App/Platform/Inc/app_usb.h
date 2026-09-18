#ifndef APP_USB_H
#define APP_USB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册 USB CDC 驱动并初始化为虚拟串口（USB0，HS）。
 */
void app_usb_init(void);

/**
 * @brief 发送数据（100ms 超时；未枚举/未打开端口时返回 -1）。
 * @return 0 成功，-1 失败
 */
int app_usb_write(const uint8_t *data, size_t len);

/**
 * @brief 发送以 '\0' 结尾的字符串。
 * @return 0 成功，-1 失败
 */
int app_usb_write_str(const char *str);

#ifdef __cplusplus
}
#endif

#endif /* APP_USB_H */
