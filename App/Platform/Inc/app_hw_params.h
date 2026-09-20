/*
 * App HW Params - 硬件电路参数（工厂默认，来源 config/hardware.yaml）
 *
 * 消费者：app_analog_signal / app_adc / app_3phase_inverter / app_fault（标度）/ app_debug_inverter / app_debug_motor。
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_HW_PARAMS_H
#define APP_HW_PARAMS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float shunt_ohm;  /* 采样电阻 [Ω] */
    float amp_gain;   /* 运放增益 */
    float a_per_volt; /* 电流标度 [A/V]（= 1/(shunt×gain)，派生） */
    float bias_v;     /* 零电流偏置 [V]（标称 vref/2；vref 由驱动侧固定 3.3V） */
} app_hw_current_sense_t;

typedef struct {
    uint32_t divider_high_ohm; /* 分压上臂 [Ω] */
    uint32_t divider_low_ohm;  /* 分压下臂 [Ω] */
    float    v_per_volt;       /* 母线标度 [V/V]（派生） */
} app_hw_vbus_sense_t;

typedef struct {
    uint32_t pullup_ohm; /* 板上上拉 [Ω] */
    float    r25_ohm;    /* NTC 25°C 阻值 [Ω]（占位，待选型） */
    float    b_value_k;  /* B 常数 [K]（占位，待选型） */
    float    max_ohm;    /* 开路/超量程替代值 [Ω] */
} app_hw_ntc_t;

typedef struct {
    uint8_t levels; /* 拨码档数（占位；解码未实现） */
} app_hw_canid_t;

typedef struct {
    uint8_t  sample_cycle;    /* 采样窗口 [ADC 时钟数]（SDK 最小 10，勿低于） */
    uint32_t trigger_delay_ns;/* 谷底后触发延时 [ns] */
} app_hw_adc_t;

typedef struct {
    uint32_t pwm_freq_hz; /* 开关频率 [Hz] */
    uint32_t deadtime_ns; /* HPM 侧死区 [ns] */
} app_hw_inverter_t;

typedef struct {
    app_hw_current_sense_t current_sense;
    app_hw_vbus_sense_t    vbus_sense;
    app_hw_ntc_t           ntc;
    app_hw_canid_t         canid_dip;
    app_hw_adc_t           adc;
    app_hw_inverter_t      inverter;
} app_hw_params_t;

/** @brief 工厂默认参数（只读，指向生成常量） */
const app_hw_params_t *app_hw_params_default(void);

/** @brief 加载参数：工厂默认 +（将来）flash 覆盖 */
void app_hw_params_load(app_hw_params_t *out);

#ifdef __cplusplus
}
#endif

#endif /* APP_HW_PARAMS_H */
