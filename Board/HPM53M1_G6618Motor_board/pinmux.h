/*
 * Copyright (c) 2026 HPMicro
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 板级 pinmux 声明 —— HPM53M1_G6618Motor_board
 * 实现与引脚分配说明见 pinmux.c。
 */

#ifndef HPM_PINMUX_H
#define HPM_PINMUX_H

#include "hpm_soc.h"

#ifdef __cplusplus
extern "C" {
#endif

void init_jtag_pins(void);
void init_uart0_pins(void);
void init_mcan3_pins(void);
void init_spi1_pins(void);
void init_spi3_pins(void);
void init_analog_pins(void);
void init_motor_driver_pins(void);
void init_gpio_pins(void);

void init_pins(void);

/* compatibility wrapper */
void init_uart_pins(UART_Type *ptr);

#ifdef __cplusplus
}
#endif

#endif /* HPM_PINMUX_H */
