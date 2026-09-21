/**
 * @file    app_3phase_inverter.h
 * @brief   三相逆变桥平台封装（PWM1 → 合封预驱 → 三相半桥）
 * @author  Kaiser
 *
 * 相映射（与板级 pinmux 一致）：
 *   U = PWM1 ch4/5（PA20/21 → HIN1/LIN1）
 *   V = PWM1 ch6/7（PA22/23 → HIN2/LIN2）
 *   W = PWM1 ch0/1（PA24/25 → HIN3/LIN3）
 *
 * 占空比语义：0.0 ~ 1.0；0.5 = 零电压矢量（半桥中点平均 = Vbus/2）。
 * 开关频率：默认 25kHz（选型依据见 app_3phase_inverter.c）。
 *
 * 安全模型：
 *   - 初始化后输出保持关闭（PWM 未使能、+12V 关闭）
 *   - enable：先开 +12V 栅极供电 → 再启动三相 PWM
 *   - disable：先停 PWM → 再关 +12V
 *   - emergency_stop：三相强制关断（force low）+ 关 +12V
 *   - force_low 与占空比更新互不干扰（独立 force 通路），可安全用于保护
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_3PHASE_INVERTER_H
#define APP_3PHASE_INVERTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 三相桥臂标识
 */
typedef enum {
    APP_3PHASE_U = 0, /**< U 相 */
    APP_3PHASE_V,     /**< V 相 */
    APP_3PHASE_W,     /**< W 相 */
    APP_3PHASE_COUNT
} app_3phase_id_t;

/**
 * @brief 初始化配置
 *
 * 默认值来源 config/hardware.yaml → app_hardware_params.inverter；
 * 更换 MOS/驱动电路后调整 YAML 中的 deadtime_ns。
 */
typedef struct {
    uint32_t pwm_freq_hz; /**< 开关频率 [Hz]（YAML: inverter.pwm_freq_hz） */
    uint32_t deadtime_ns; /**< 死区 [ns]（YAML: inverter.deadtime_ns） */
} app_3phase_inverter_cfg_t;

/**
 * @brief 初始化三相逆变桥：按配置设置三相 PWM（频率/死区/中心对齐），输出保持关闭。
 * @param cfg 配置；NULL = 使用默认值（config/hardware.yaml → app_hardware_params.inverter）
 */
void app_3phase_inverter_init(const app_3phase_inverter_cfg_t* cfg);

/**
 * @brief 使能输出（+12V 先开 → 启动三相 PWM）。
 * @return 0 成功，-1 失败
 */
int app_3phase_inverter_enable(void);

/**
 * @brief 关闭输出（先停 PWM → 关 +12V）。
 */
void app_3phase_inverter_disable(void);

/**
 * @brief 三相占空比更新（热路径：无打印/无动态分配，逐相限幅 [0,1]）。
 * @return 0 成功，-1 参数/状态错误
 */
int app_3phase_inverter_set_duty_abc(float duty_u, float duty_v, float duty_w);

/**
 * @brief 单相占空比更新（调试用）。
 * @return 0 成功，-1 失败
 */
int app_3phase_inverter_set_duty(app_3phase_id_t phase, float duty);

/**
 * @brief 单相强制关断（桥臂高阻；调试/保护用）。与 set_duty 互不干扰。
 * @return 0 成功，-1 失败
 */
int app_3phase_inverter_force_low(app_3phase_id_t phase);

/**
 * @brief 单相解除强制关断（恢复该相输出）。
 * @return 0 成功，-1 失败
 */
int app_3phase_inverter_release(app_3phase_id_t phase);

/**
 * @brief 紧急停止：三相强制关断 + 关 +12V。
 */
void app_3phase_inverter_emergency_stop(void);

/**
 * @brief 当前是否已使能输出。
 */
bool app_3phase_inverter_is_enabled(void);

/**
 * @brief 读取当前三相占空比（可为 NULL 跳过对应相）。
 */
void app_3phase_inverter_get_duty_abc(float* duty_u, float* duty_v, float* duty_w);

#ifdef __cplusplus
}
#endif

#endif /* APP_3PHASE_INVERTER_H */
