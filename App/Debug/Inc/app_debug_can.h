/**
 * @file    app_debug_can.h
 * @brief   CAN 自检（内部环回 + 经典 CAN 总线收发）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_DEBUG_CAN_H
#define APP_DEBUG_CAN_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief CAN 自检初始化：
 *        1) 内部环回自检（验证控制器 / 消息 RAM / 过滤器 / 中断链路，无需外部节点）
 *        2) 以经典 CAN 模式初始化 MCAN3（正常运行波特率来源 config/software.yaml；
 *           环回自检固定 1Mbps）
 *        3) 配置接收过滤器（全接收）与 RX 回调
 */
void app_debug_can_init(void);

/**
 * @brief CAN 自检周期任务（只读）：每秒打印驱动/协议统计
 *
 * RX 分发与周期回报已移交 App/Comm/can（app_can_service_step）；
 * 本函数不再发送任何报文，也不注册回调。
 */
void app_debug_can_run_once(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_DEBUG_CAN_H */
