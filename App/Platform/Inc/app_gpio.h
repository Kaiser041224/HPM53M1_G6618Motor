#ifndef APP_GPIO_H
#define APP_GPIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pin definitions (must match Board/HPM53M1_G6618Motor_board/pinmux.c) */
/*
 * PA09: 栅极驱动器 +12V 供电使能（DRV_+12V_EN）
 *   原理图为 15K 下拉（默认关闭）；当前测试板 15K 接 5V 作上拉（默认使能），
 *   定稿改回下拉。
 *   当前按上拉配置为开漏：写 0 = 关闭；写 1 = 释放（使能）。
 *   定稿改为下拉后需切换为推挽输出（写 1 = 使能）。
 */
#define PIN_GDRV_12V_EN ((0 << 5) | 9)

/* PB01: 状态 LED（D9，低电平点亮）
 * 注意：HPM53M1 上 PB01 = ADCIN14（封装 pin 43），为纯模拟端口、无 GPIO 功能，
 *       该 LED 无法由固件驱动（HPM5361 迁移遗留），需改板至 PA 引脚。 */
#define PIN_LED_STATUS  ((1 << 5) | 1)

void app_gpio_init(void);
void app_gpio_set(uint16_t pin, uint8_t on);
void app_gpio_toggle(uint16_t pin);
uint8_t app_gpio_read(uint16_t pin);

#ifdef __cplusplus
}
#endif

#endif /* APP_GPIO_H */
