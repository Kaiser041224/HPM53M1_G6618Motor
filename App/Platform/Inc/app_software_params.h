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
    uint8_t shutdown_en;    /**< 1=故障触发停机（FAULT 锁存）；0=台架模式：仅检测/告警不停机 */
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
 * @brief 控制限幅参数（FOC 电流环消费）
 */
typedef struct {
    float i_q_max_a; /**< 电流限幅 [A]（峰值口径 = 相电流峰值） */
    float duty_max;  /**< 调制上限（有效域 (0.5, 1.0]；FOC 电流环消费） */
    float i_trip_a;  /**< 快速过流跳闸 [A]（峰值口径；0 = 关闭） */
    float speed_max_rad_s; /**< 转矩模式限速 [rad/s 电角]（0 = 关闭） */
} app_software_limits_t;

/**
 * @brief 电流环参数
 */
typedef struct {
    float   kp;              /**< 比例增益 [V/A] */
    float   ki;              /**< 积分增益 [V/(A·s)] */
    float   bandwidth_rad_s; /**< 目标带宽 [rad/s]（自整定用） */
    uint8_t decoupling_en;   /**< 解耦前馈（0/1） */
} app_software_current_loop_t;

/**
 * @brief 控制环参数
 */
typedef struct {
    app_software_current_loop_t current_loop; /**< 电流环 */
    app_software_pid_t          speed_loop;   /**< 速度环（V2） */
    app_software_limits_t       limits;       /**< 控制限幅 */
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

/** @brief 初始化运行期单例（boot 时调用一次；幂等） */
void app_software_params_init(void);

/** @brief 运行期参数单例（只读；消费者统一经此读取；未初始化时自动初始化） */
const app_software_params_t* app_software_params_current(void);

/** @brief 运行期参数单例（可写；仅 Shell param 命令等调试路径使用） */
app_software_params_t* app_software_params_mutable(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_SW_PARAMS_H */
