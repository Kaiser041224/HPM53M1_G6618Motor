/**
 * @file    app_hardware_params.h
 * @brief   硬件电路参数（工厂默认，来源 config/hardware.yaml）
 * @author  Kaiser
 *
 * 消费者：app_analog_signal / app_adc / app_3phase_inverter / app_fault（标度）/ app_debug_inverter
 * / app_debug_motor。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_HW_PARAMS_H
#define APP_HW_PARAMS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 电流采样链路参数
 */
typedef struct {
    float shunt_ohm;  /**< 采样电阻 [Ω] */
    float amp_gain;   /**< 运放增益 */
    float a_per_volt; /**< 电流标度 [A/V]（= 1/(shunt×gain)，派生） */
    float bias_v;     /**< 零电流偏置 [V]（标称 vref/2；vref 由驱动侧固定 3.3V） */
    uint8_t invert;   /**< 1 = 反相为"流入电机为正"（本板硬件反相；见 YAML 注释） */
} app_hardware_current_sense_t;

/**
 * @brief 母线电压采样参数
 */
typedef struct {
    uint32_t divider_high_ohm; /**< 分压上臂 [Ω] */
    uint32_t divider_low_ohm;  /**< 分压下臂 [Ω] */
    float v_per_volt;          /**< 母线标度 [V/V]（派生） */
} app_hardware_vbus_sense_t;

/**
 * @brief NTC 温度采样参数
 */
typedef struct {
    uint32_t pullup_ohm; /**< 板上上拉 [Ω] */
    float r25_ohm;       /**< NTC 25°C 阻值 [Ω]（占位，待选型） */
    float b_value_k;     /**< B 常数 [K]（占位，待选型） */
    float max_ohm;       /**< 开路/超量程替代值 [Ω] */
} app_hardware_ntc_t;

/**
 * @brief CANID 拨码参数
 */
typedef struct {
    uint8_t levels; /**< 拨码档数（占位；解码未实现） */
} app_hardware_canid_t;

/**
 * @brief ADC 采样参数
 */
typedef struct {
    uint8_t sample_cycle;      /**< 采样窗口 [ADC 时钟数]（SDK 最小 10，勿低于） */
    uint32_t trigger_delay_ns; /**< 谷底后触发延时 [ns] */
} app_hardware_adc_t;

/**
 * @brief 逆变器参数
 */
typedef struct {
    uint32_t pwm_freq_hz; /**< 开关频率 [Hz] */
    uint32_t deadtime_ns; /**< HPM 侧死区 [ns] */
} app_hardware_inverter_t;

/**
 * @brief 硬件电路参数（工厂默认）
 */
typedef struct {
    app_hardware_current_sense_t current_sense; /**< 电流采样链路 */
    app_hardware_vbus_sense_t vbus_sense;       /**< 母线电压采样 */
    app_hardware_ntc_t ntc;                     /**< NTC 温度采样 */
    app_hardware_canid_t canid_dip;             /**< CANID 拨码 */
    app_hardware_adc_t adc;                     /**< ADC 采样 */
    app_hardware_inverter_t inverter;           /**< 逆变器 */
} app_hardware_params_t;

/** @brief 工厂默认参数（只读，指向生成常量） */
const app_hardware_params_t* app_hardware_params_default(void);

/** @brief 加载参数：工厂默认 +（将来）flash 覆盖 */
void app_hardware_params_load(app_hardware_params_t* out);

/** @brief 初始化运行期单例（boot 时调用一次；幂等） */
void app_hardware_params_init(void);

/** @brief 运行期参数单例（只读；消费者统一经此读取；未初始化时自动初始化） */
const app_hardware_params_t* app_hardware_params_current(void);

/** @brief 运行期参数单例（可写；仅 Shell param 命令等调试路径使用） */
app_hardware_params_t* app_hardware_params_mutable(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_HW_PARAMS_H */
