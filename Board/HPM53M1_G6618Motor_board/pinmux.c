/**
 * @file    pinmux.c
 * @brief   板级 pinmux —— HPM53M1_G6618Motor_board
 * @author  Kaiser
 *
 * 依据：
 *   - HPM Pinmux Tool 输出（实际板级引脚定义，原始文件见 Doc/datasheet/pinmux.c）
 *   - HPM53M1 数据手册 Rev0.3（QFN80）
 *
 * 说明：
 *   - HPM53M1 的 GPIO 仅 PA00~PA15、PA26~PA29；PB00/PB01、PB08~PB14 为 die 侧
 *     模拟 pad，在 QFN80 上以 ADCIN1~6/11/14/15 引出（本文件按 die pad 名配置）。
 *   - PWM1 已由芯片内部连接至合封三相半桥预驱（datasheet 1.3.7 节），
 *     内部走线 PA20/22/24 -> HIN1/2/3、PA21/23/25 -> LIN1/2/3。
 *     这些 pad 不对外引出，但仍需配置 IOC 把 PWM1 送到预驱，
 *     见 init_motor_driver_pins()。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pinmux.h"
#include "board.h"
#include "hpm_gpio_drv.h"
#include "hpm_gpiom_drv.h"

/**
 * @brief 配置 JTAG 引脚（PA04~PA08）
 */
void init_jtag_pins(void) {
    HPM_IOC->PAD[IOC_PAD_PA04].FUNC_CTL = IOC_PA04_FUNC_CTL_JTAG_TDO;

    HPM_IOC->PAD[IOC_PAD_PA05].FUNC_CTL = IOC_PA05_FUNC_CTL_JTAG_TDI;

    HPM_IOC->PAD[IOC_PAD_PA06].FUNC_CTL = IOC_PA06_FUNC_CTL_JTAG_TCK;

    HPM_IOC->PAD[IOC_PAD_PA07].FUNC_CTL = IOC_PA07_FUNC_CTL_JTAG_TMS;

    HPM_IOC->PAD[IOC_PAD_PA08].FUNC_CTL = IOC_PA08_FUNC_CTL_JTAG_TRST;
}

/**
 * @brief 配置 UART0 引脚（PA00 TXD / PA01 RXD）
 */
void init_uart0_pins(void) {
    /* PA00: UART0_TXD, PA01: UART0_RXD */
    HPM_IOC->PAD[IOC_PAD_PA00].FUNC_CTL = IOC_PA00_FUNC_CTL_UART0_TXD;

    HPM_IOC->PAD[IOC_PAD_PA01].FUNC_CTL = IOC_PA01_FUNC_CTL_UART0_RXD;
}

/**
 * @brief 配置 MCAN3 引脚（PA15 TXD / PA14 RXD）
 */
void init_mcan3_pins(void) {
    /* PA15: MCAN3_TXD, PA14: MCAN3_RXD */
    HPM_IOC->PAD[IOC_PAD_PA15].FUNC_CTL = IOC_PA15_FUNC_CTL_MCAN3_TXD;

    HPM_IOC->PAD[IOC_PAD_PA14].FUNC_CTL = IOC_PA14_FUNC_CTL_MCAN3_RXD;
}

/**
 * @brief 配置 SPI1 引脚（PA26 CS / PA27 SCLK / PA28 MISO / PA29 MOSI）
 */
void init_spi1_pins(void) {
    /* SPI1: PA26 CS_0, PA27 SCLK, PA28 MISO, PA29 MOSI */
    HPM_IOC->PAD[IOC_PAD_PA26].FUNC_CTL = IOC_PA26_FUNC_CTL_SPI1_CS_0;

    HPM_IOC->PAD[IOC_PAD_PA27].FUNC_CTL = IOC_PA27_FUNC_CTL_SPI1_SCLK | IOC_PAD_FUNC_CTL_LOOP_BACK_MASK;

    HPM_IOC->PAD[IOC_PAD_PA28].FUNC_CTL = IOC_PA28_FUNC_CTL_SPI1_MISO;

    HPM_IOC->PAD[IOC_PAD_PA29].FUNC_CTL = IOC_PA29_FUNC_CTL_SPI1_MOSI;
}

/**
 * @brief 配置 SPI3 引脚（PA10 CS / PA11 SCLK / PA12 MISO / PA13 MOSI）
 */
void init_spi3_pins(void) {
    /* SPI3: PA10 CS_0, PA11 SCLK, PA12 MISO, PA13 MOSI */
    HPM_IOC->PAD[IOC_PAD_PA10].FUNC_CTL = IOC_PA10_FUNC_CTL_SPI3_CS_0;

    HPM_IOC->PAD[IOC_PAD_PA11].FUNC_CTL = IOC_PA11_FUNC_CTL_SPI3_SCLK | IOC_PAD_FUNC_CTL_LOOP_BACK_MASK;

    HPM_IOC->PAD[IOC_PAD_PA12].FUNC_CTL = IOC_PA12_FUNC_CTL_SPI3_MISO;

    HPM_IOC->PAD[IOC_PAD_PA13].FUNC_CTL = IOC_PA13_FUNC_CTL_SPI3_MOSI;
}

/**
 * @brief 配置模拟输入 pad（PB00/PB01/PB08~PB14）
 */
void init_analog_pins(void) {
    /* 模拟输入 pad（PB00/PB01、PB08~PB14） */
    HPM_IOC->PAD[IOC_PAD_PB14].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;

    HPM_IOC->PAD[IOC_PAD_PB12].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;

    HPM_IOC->PAD[IOC_PAD_PB11].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;

    HPM_IOC->PAD[IOC_PAD_PB10].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;

    HPM_IOC->PAD[IOC_PAD_PB09].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;

    HPM_IOC->PAD[IOC_PAD_PB08].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;

    /* PB00: ADC_V_CANID（DIP 开关电阻编码，模拟读取） */
    HPM_IOC->PAD[IOC_PAD_PB00].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;
}

/**
 * @brief 配置合封三相半桥预驱的内部走线（pad 不对外引出，仅需配置 IOC 选通 PWM1）
 *
 *   PA20 -> HIN1 = PWM1_P_4      PA21 -> LIN1 = PWM1_P_5
 *   PA22 -> HIN2 = PWM1_P_6      PA23 -> LIN2 = PWM1_P_7
 *   PA24 -> HIN3 = PWM1_P_0      PA25 -> LIN3 = PWM1_P_1
 *
 * 注意：此映射依据 datasheet 封装图内部连线 + IOMUX 功能推导，
 *       建议对照《关节驱动参考电路设计》或实测确认后再量产。
 */
void init_motor_driver_pins(void) {
    HPM_IOC->PAD[IOC_PAD_PA20].FUNC_CTL = IOC_PA20_FUNC_CTL_PWM1_P_4;

    HPM_IOC->PAD[IOC_PAD_PA21].FUNC_CTL = IOC_PA21_FUNC_CTL_PWM1_P_5;

    HPM_IOC->PAD[IOC_PAD_PA22].FUNC_CTL = IOC_PA22_FUNC_CTL_PWM1_P_6;

    HPM_IOC->PAD[IOC_PAD_PA23].FUNC_CTL = IOC_PA23_FUNC_CTL_PWM1_P_7;

    HPM_IOC->PAD[IOC_PAD_PA24].FUNC_CTL = IOC_PA24_FUNC_CTL_PWM1_P_0;

    HPM_IOC->PAD[IOC_PAD_PA25].FUNC_CTL = IOC_PA25_FUNC_CTL_PWM1_P_1;
}

/**
 * @brief 配置 GPIO 引脚（PA09 预驱供电使能、PB01 状态 LED）
 */
void init_gpio_pins(void) {
    /*
     * PA09: 栅极驱动器 +12V 供电使能（DRV_+12V_EN，控制 U6 升压 EN）
     *   原理图设计为 15K 下拉（默认关闭）；当前测试板将该 15K 焊至 5V
     *   作为上拉（默认使能），定稿将改回下拉。
     *   当前按上拉状态配置为开漏：写 0 = 下拉关闭；写 1 = 释放（上拉使能）。
     *   注意：定稿改为下拉后需切换为推挽输出（写 1 = 使能），否则无法使能。
     *   上电默认输出 0（+12V 关闭），由应用在启动电机前使能。
     */
    HPM_IOC->PAD[IOC_PAD_PA09].FUNC_CTL = IOC_PA09_FUNC_CTL_GPIO_A_09;
    HPM_IOC->PAD[IOC_PAD_PA09].PAD_CTL = IOC_PAD_PAD_CTL_OD_SET(1);

    gpiom_set_pin_controller(HPM_GPIOM, GPIOM_ASSIGN_GPIOA, 9, gpiom_soc_gpio0);
    gpio_set_pin_output(HPM_GPIO0, GPIO_OE_GPIOA, 9);
    gpio_write_pin(HPM_GPIO0, GPIO_DO_GPIOA, 9, 0);

    /*
     * PB01: 状态 LED（D9，低电平点亮）
     * 注意：HPM53M1 上 PB01 = 封装 pin 43 (ADCIN14)，为纯模拟端口，
     *       无数字/GPIO 功能（datasheet §2.5：GPIO 全部在 PA 组）。
     *       以下配置在 HPM53M1 上无效（对 HPM5361 有效），LED 需改板至 PA 引脚。
     */
    HPM_IOC->PAD[IOC_PAD_PB01].FUNC_CTL = IOC_PB01_FUNC_CTL_GPIO_B_01;

    gpiom_set_pin_controller(HPM_GPIOM, GPIOM_ASSIGN_GPIOB, 1, gpiom_soc_gpio0);
    gpio_set_pin_output(HPM_GPIO0, GPIO_OE_GPIOB, 1);
    gpio_write_pin(HPM_GPIO0, GPIO_DO_GPIOB, 1, 1);
}

/**
 * @brief 按序初始化所有板级引脚
 */
void init_pins(void) {
    init_jtag_pins();
    init_uart0_pins();
    init_mcan3_pins();
    init_spi1_pins();
    init_spi3_pins();
    init_analog_pins();
    init_motor_driver_pins();
    init_gpio_pins();
}

/**
 * @brief UART 引脚兼容包装（当前仅支持 UART0）
 * @param ptr UART 实例指针
 */
void init_uart_pins(UART_Type* ptr) {
    if (ptr == HPM_UART0) {
        init_uart0_pins();
    }
}
