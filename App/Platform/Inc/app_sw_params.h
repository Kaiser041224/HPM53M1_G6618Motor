/*
 * App SW Params - 软件参数（工厂默认，来源 config/software.yaml）
 *
 * 消费者：app_fault（阈值）/ app_can（波特率）/ app_debug_can（回报帧 ID）/ FOC（后续，PID 与限幅）。
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_SW_PARAMS_H
#define APP_SW_PARAMS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t baudrate;        /* CAN 波特率 [bps] */
    uint8_t  node_id_default; /* 默认节点号（占位：DIP 解码未实现；与 CAN ID 的派生关系待协议定稿） */
    uint32_t rx_control_id;   /* 接收控制帧 CAN ID（占位：待协议定稿） */
    uint32_t tx_report_id;    /* 参数回报帧 CAN ID（占位：待协议定稿） */
} app_sw_can_t;

typedef struct {
    float    oc_trip_a;     /* 过流阈值 [A] */
    float    vbus_ov_v;     /* 母线过压 [V] */
    float    vbus_uv_v;     /* 母线欠压 [V] */
    uint16_t slow_debounce; /* L2/L3 去抖次数（1kHz） */
    uint16_t adc_stall_ms;  /* PMT 帧停滞超时 [ms] */
    uint8_t  enc_err_delta; /* 编码器错误增量阈值 */
} app_sw_fault_t;

typedef struct {
    float kp;
    float ki;
} app_sw_pid_t;

typedef struct {
    float i_q_max_a; /* 电流限幅 [A]（RMS 口径；FOC 预留，未消费） */
    float duty_max;  /* 占空比上限（FOC 预留，未消费） */
} app_sw_limits_t;

typedef struct {
    app_sw_pid_t    current_loop; /* 电流环（FOC 预留） */
    app_sw_pid_t    speed_loop;   /* 速度环（FOC 预留） */
    app_sw_limits_t limits;
} app_sw_control_t;

typedef struct {
    app_sw_can_t     can;
    app_sw_fault_t   fault;
    app_sw_control_t control;
} app_sw_params_t;

/** @brief 工厂默认参数（只读，指向生成常量） */
const app_sw_params_t *app_sw_params_default(void);

/** @brief 加载参数：工厂默认 +（将来）flash 覆盖（整定/自校准结果） */
void app_sw_params_load(app_sw_params_t *out);

#ifdef __cplusplus
}
#endif

#endif /* APP_SW_PARAMS_H */
