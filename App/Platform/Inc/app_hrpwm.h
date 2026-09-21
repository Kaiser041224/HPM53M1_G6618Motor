/**
 * @file    app_hrpwm.h
 * @brief   HRPWM 平台封装（成对 PWM 通道、频率/死区/相移控制）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_HRPWM_H
#define APP_HRPWM_H

#include <stdint.h>

/* 热路径段属性：映射到链接脚本 .fast（ILM）段（等价 SDK ATTR_RAMFUNC；避免 App 直接包含 SDK 头） */
#define APP_HRPWM_ATTR_RAMFUNC __attribute__((section(".fast")))
/* 初始化段属性：映射到 .fast_ram.init 段（启动搬运后释放） */
#define APP_HRPWM_ATTR_FAST_RAM_INIT __attribute__((section(".fast_ram.init")))

#ifdef __cplusplus
extern "C" {
#endif

#define APP_HRPWM_DEFAULT_FREQ_HZ (200000U) /**< 默认开关频率 [Hz] */

/**
 * @brief HRPWM 配对标识
 */
typedef enum {
    HRPWM_PAIR_A = 0, /**< PWM0 ch0/1 */
    HRPWM_PAIR_B,     /**< PWM0 ch2/3 */
    HRPWM_PAIR_C,     /**< PWM1 ch4/5（PA20/21 → HIN1/LIN1，U 相） */
    HRPWM_PAIR_D,     /**< PWM1 ch6/7（PA22/23 → HIN2/LIN2，V 相） */
    HRPWM_PAIR_E,     /**< PWM1 ch0/1（PA24/25 → HIN3/LIN3，W 相） */
    HRPWM_PAIR_COUNT,
} hrpwm_pair_t;

/**
 * @brief HRPWM 实例标识
 */
typedef enum {
    HRPWM_INST_0 = 0, /**< PWM0 */
    HRPWM_INST_1,     /**< PWM1 */
    HRPWM_INST_COUNT,
} hrpwm_inst_t;

/**
 * @brief 初始化 HRPWM 平台
 */
void app_hrpwm_init(void);

/**
 * @brief 设置配对占空比（经相移/限幅约束）
 * @param pair 配对
 * @param duty 占空比 [0.0, 1.0]
 */
void app_hrpwm_set_duty(hrpwm_pair_t pair, float duty);

/**
 * @brief 直接设置配对占空比（不经约束）
 * @param pair 配对
 * @param duty 占空比 [0.0, 1.0]
 */
void app_hrpwm_set_duty_direct(hrpwm_pair_t pair, float duty);

/**
 * @brief 同时直接设置两个配对占空比
 * @param pair_a 配对 A
 * @param duty_a 配对 A 占空比 [0.0, 1.0]
 * @param pair_b 配对 B
 * @param duty_b 配对 B 占空比 [0.0, 1.0]
 */
void app_hrpwm_set_duty_direct_dual(
    hrpwm_pair_t pair_a, float duty_a, hrpwm_pair_t pair_b, float duty_b);

/**
 * @brief 设置实例开关频率
 * @param inst 实例
 * @param freq_hz 频率 [Hz]
 */
void app_hrpwm_set_frequency(hrpwm_inst_t inst, uint32_t freq_hz);

/**
 * @brief 设置配对抖动比较值
 * @param pair 配对
 * @param jitter_cmp 抖动比较值
 */
void app_hrpwm_set_jitter(hrpwm_pair_t pair, uint8_t jitter_cmp);

/**
 * @brief 启动配对输出
 * @param pair 配对
 */
void app_hrpwm_start(hrpwm_pair_t pair);

/**
 * @brief 停止配对输出
 * @param pair 配对
 */
void app_hrpwm_stop(hrpwm_pair_t pair);

/**
 * @brief 停止全部配对输出
 */
void app_hrpwm_stop_all(void);

/**
 * @brief 启动全部配对输出
 */
void app_hrpwm_start_all(void);

/**
 * @brief 仅启动指定实例的 PWM 计数器 (CEN)，不使能物理输出。
 *
 * 用于软启动前预热 ADC PMT 触发链：CMP 事件依赖计数器运行，
 * 待占空比计算完成后再调用 app_hrpwm_start() 使能实际输出。
 * @param inst 实例
 */
void app_hrpwm_start_counter_only(hrpwm_inst_t inst);

/**
 * @brief 按配置重配一个配对（频率/死区），占空比归零、中心对齐、不反相。
 *
 * 用于逆变器等模块按板级/参数配置初始化（如 YAML 参数管线提供的开关频率与死区）。
 * 注意：应在配对未启动（输出关闭）时调用，如初始化阶段。
 * @param pair 配对
 * @param frequency_hz 频率 [Hz]
 * @param deadtime_ns 死区 [ns]
 * @return 0 成功，-1 失败
 */
int app_hrpwm_config_pair(hrpwm_pair_t pair, uint32_t frequency_hz, uint32_t deadtime_ns);

/**
 * @brief 强制配对输出低电平
 * @param pair 配对
 */
void app_hrpwm_force_low(hrpwm_pair_t pair);

/**
 * @brief 解除配对强制低电平
 * @param pair 配对
 */
void app_hrpwm_force_release(hrpwm_pair_t pair);

/**
 * @brief 紧急停止全部输出
 */
void app_hrpwm_emergency_stop(void);

/**
 * @brief 从紧急停止状态恢复
 */
void app_hrpwm_resume(void);

/**
 * @brief 配置硬件故障保护
 */
void app_hrpwm_config_fault(void);

/**
 * @brief 清除硬件故障
 */
void app_hrpwm_clear_fault(void);

/**
 * @brief 设置实例内配对间相移
 * @param inst 实例
 * @param ref_pair 参考配对
 * @param target_pair 目标配对
 * @param phase_deg 相移 [度]
 */
void app_hrpwm_set_phase(hrpwm_inst_t inst, uint8_t ref_pair, uint8_t target_pair, float phase_deg);

/**
 * @brief 配置相移限幅
 * @param inst 实例
 * @param max_phase_deg 最大相移 [度]
 * @param max_duty_ref 参考配对最大占空比
 * @param max_duty_target 目标配对最大占空比
 */
void app_hrpwm_config_phase_limit(
    hrpwm_inst_t inst, float max_phase_deg, float max_duty_ref, float max_duty_target);

#ifdef __cplusplus
}
#endif

#endif /* APP_HRPWM_H */
