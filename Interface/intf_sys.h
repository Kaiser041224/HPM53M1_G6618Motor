/**
 * @file    intf_sys.h
 * @brief   系统级信息与临界区抽象接口
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef INTF_SYS_H
#define INTF_SYS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 复位源位定义（PPOR RESET_STATUS，只读；与 RESET_FLAG 位定义一致）。
 * 注意：PPOR.RESET_FLAG 为 write-only 寄存器（datasheet/SVD），不可读，
 *      复位历史无法通过读取获取；RESET_STATUS 只反映“当前仍有效的复位源”。
 */
#define INTF_SYS_RST_BROWNOUT  (1UL << 0U)  /**< 欠压 */
#define INTF_SYS_RST_TEMP      (1UL << 1U)  /**< 温度 */
#define INTF_SYS_RST_DEBUG     (1UL << 4U)  /**< 调试复位 */
#define INTF_SYS_RST_JTAG_SOFT (1UL << 5U)  /**< JTAG 软复位 */
#define INTF_SYS_RST_WDOG0     (1UL << 16U) /**< 看门狗 0 */
#define INTF_SYS_RST_WDOG1     (1UL << 17U) /**< 看门狗 1 */
#define INTF_SYS_RST_PMIC_WDOG (1UL << 24U) /**< 电源域看门狗 */
#define INTF_SYS_RST_JTAG_IEEE (1UL << 30U) /**< JTAG IEEE 复位 */
#define INTF_SYS_RST_SOFTWARE  (1UL << 31U) /**< 软件复位 */

/**
 * @brief 读取当前仍有效的复位源状态
 * @return 复位源位掩码（INTF_SYS_RST_* 取或）
 */
uint32_t intf_sys_get_reset_status(void);

/* 全局中断临界区（App 层不得直接访问 CSR；仅用于短临界区，须成对使用）。
 * intf_sys_irq_save 返回保存的中断状态，intf_sys_irq_restore 恢复。 */

/**
 * @brief 关闭全局中断并返回保存状态
 * @return 进入临界区前的中断状态
 */
uint32_t intf_sys_irq_save(void);

/**
 * @brief 恢复中断使能状态（与 intf_sys_irq_save 配对）。
 * @param state intf_sys_irq_save 返回的状态
 */
void intf_sys_irq_restore(uint32_t state);

/**
 * @brief 软件复位（PPOR SOFTWARE_RESET；调用后立即复位，不返回）。
 *
 * 用途：Shell reboot 命令。调用前须确保电机停止等安全前置条件。
 */
void intf_sys_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* INTF_SYS_H */
