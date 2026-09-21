/**
 * @file    pinmux.h
 * @brief   板级 pinmux 声明 —— HPM53M1_G6618Motor_board
 * @author  Kaiser
 *
 * 实现与引脚分配说明见 pinmux.c。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef HPM_PINMUX_H
#define HPM_PINMUX_H

#include "hpm_soc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 配置 JTAG 引脚（PA04~PA08）
 */
void init_jtag_pins(void);

/**
 * @brief 配置 UART0 引脚（PA00 TXD / PA01 RXD）
 */
void init_uart0_pins(void);

/**
 * @brief 配置 MCAN3 引脚（PA15 TXD / PA14 RXD）
 */
void init_mcan3_pins(void);

/**
 * @brief 配置 SPI1 引脚（PA26 CS / PA27 SCLK / PA28 MISO / PA29 MOSI）
 */
void init_spi1_pins(void);

/**
 * @brief 配置 SPI3 引脚（PA10 CS / PA11 SCLK / PA12 MISO / PA13 MOSI）
 */
void init_spi3_pins(void);

/**
 * @brief 配置模拟输入 pad（PB00/PB01/PB08~PB14）
 */
void init_analog_pins(void);

/**
 * @brief 配置合封三相半桥预驱的内部走线（pad 不对外引出，仅需配置 IOC 选通 PWM1）
 */
void init_motor_driver_pins(void);

/**
 * @brief 配置 GPIO 引脚（PA09 预驱供电使能、PB01 状态 LED）
 */
void init_gpio_pins(void);

/**
 * @brief 按序初始化所有板级引脚
 */
void init_pins(void);

/**
 * @brief UART 引脚兼容包装（当前仅支持 UART0）
 * @param ptr UART 实例指针
 */
void init_uart_pins(UART_Type *ptr);

#ifdef __cplusplus
}
#endif

#endif /* HPM_PINMUX_H */
