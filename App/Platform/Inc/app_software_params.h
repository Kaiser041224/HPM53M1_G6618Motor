/**
 * @file    app_software_params.h
 * @brief   软件参数（工厂默认，来源 config/software.yaml）
 * @author  Kaiser
 *
 * 消费者：app_fault（阈值）/ app_can（波特率）/ app_debug_can（回报帧 ID）/ FOC（后续，PID
 * 与限幅）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_SW_PARAMS_H
#define APP_SW_PARAMS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief CAN 通信参数
 */
typedef struct {
    uint32_t baudrate;   /**< CAN 波特率 [bps] */
    uint8_t
        node_id_default; /**< 默认节点号（占位：DIP 解码未实现；与 CAN ID 的派生关系待协议定稿） */
    uint32_t rx_control_id; /**< 接收控制帧 CAN ID（占位：待协议定稿） */
    uint32_t tx_report_id;  /**< 参数回报帧 CAN ID（占位：待协议定稿） */
} app_software_can_t;

/**
 * @brief 故障保护阈值
 */
typedef struct {
    float oc_trip_a;        /**< 过流阈值 [A] */
    float vbus_ov_v;        /**< 母线过压 [V] */
    float vbus_uv_v;        /**< 母线欠压 [V] */
    uint16_t slow_debounce; /**< L2/L3 去抖次数（1kHz） */
    uint16_t adc_stall_ms;  /**< PMT 帧停滞超时 [ms] */
    uint8_t enc_err_delta;  /**< 编码器错误增量阈值 */
} app_software_fault_t;

/**
 * @brief PID 参数
 */
typedef struct {
    float kp; /**< 比例增益 */
    float ki; /**< 积分增益 */
} app_software_pid_t;

/**
 * @brief 控制限幅参数
 */
typedef struct {
    float i_q_max_a; /**< 电流限幅 [A]（RMS 口径；FOC 预留，未消费） */
    float duty_max;  /**< 占空比上限（FOC 预留，未消费） */
} app_software_limits_t;

/**
 * @brief 控制环参数
 */
typedef struct {
    app_software_pid_t current_loop; /**< 电流环（FOC 预留） */
    app_software_pid_t speed_loop;   /**< 速度环（FOC 预留） */
    app_software_limits_t limits;    /**< 控制限幅 */
} app_software_control_t;

/**
 * @brief 软件参数（工厂默认）
 */
typedef struct {
    app_software_can_t can;         /**< CAN 通信 */
    app_software_fault_t fault;     /**< 故障保护 */
    app_software_control_t control; /**< 控制环 */
} app_software_params_t;

/** @brief 工厂默认参数（只读，指向生成常量） */
const app_software_params_t* app_software_params_default(void);

/** @brief 加载参数：工厂默认 +（将来）flash 覆盖（整定/自校准结果） */
void app_software_params_load(app_software_params_t* out);

#ifdef __cplusplus
}
#endif

#endif /* APP_SW_PARAMS_H */
