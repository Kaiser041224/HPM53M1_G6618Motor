/**
 * @file    app_debug_flash.h
 * @brief   XPI NOR 自检
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_DEBUG_FLASH_H
#define APP_DEBUG_FLASH_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Flash 自检：初始化（auto_config）+ 属性打印 + 破坏性读写测试。
 *
 * 测试扇区 = flash 最后一个扇区（链接脚本已预留，见 board.h）。
 * 擦写前会校验该扇区为空白（全 0xFF），防止链接脚本未预留时误擦固件。
 */
void app_debug_flash_init(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_DEBUG_FLASH_H */
