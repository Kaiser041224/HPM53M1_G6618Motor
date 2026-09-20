/*
 * App Analog Signal - 物理量换算（标定 + 滤波）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 换算（依据原理图 Analog Signal Processing；系数来源 config/hardware.yaml）：
 *   电流   I [A]     = (V_adc − V_zero) × a_per_volt    （a_per_volt = 1/(shunt×gain)）
 *   母线   V_bus [V] = V_adc × v_per_volt               （v_per_volt = (Rhi+Rlo)/Rlo）
 *   NTC    R [Ω]     = pullup × V_adc / (vref − V_adc)  （pullup = hw.ntc.pullup_ohm；vref = INTF_ADC_DEFAULT_VREF_MV（驱动侧固定 3.3V））
 *
 * 电流链路为比例式：偏置（bias_v）与 ADC 基准同源，3.3V 电源漂移不影响精度。
 */

#ifndef APP_ANALOG_SIGNAL_H
#define APP_ANALOG_SIGNAL_H

#include "app_adc.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* process() 调用频率 = 主循环节拍（= inverter.pwm_freq_hz，config/hardware.yaml） */

typedef struct {
    float i_u_a;      /* 相电流 [A] */
    float i_v_a;      /* 相电流 [A] */
    float i_w_a;      /* 相电流 [A] */
    float v_bus_v;    /* 母线电压 [V] */
    float r_ntc0_ohm; /* NTC0 电阻 [Ω] */
    float r_ntc1_ohm; /* NTC1 电阻 [Ω] */
} app_analog_values_t;

/** @brief 初始化（默认零点 + 滤波器），须在 app_adc_init() 之后调用 */
void app_analog_signal_init(void);

/** @brief 25kHz 主循环每周期调用：读取原始值 → 换算 → 滤波 */
void app_analog_signal_process(void);

/** @brief 读取最新物理量快照（含滤波）；数据未就绪返回 false */
bool app_analog_signal_read_all(app_analog_values_t *values);

/** @brief 读取单通道物理量（含滤波）；数据未就绪返回 NAN */
float app_analog_signal_read(adc_channel_t ch);

/** @brief 读取单通道原始码 */
bool app_analog_signal_read_raw(adc_channel_t ch, uint16_t *raw);

/**
 * @brief 电流通道零点标定：阻塞采集若干帧求平均，写入零点偏置。
 * @note 必须在无电流状态调用（上电初始化阶段）；检测到电流或超时会拒绝。
 * @return 0 成功，-1 失败
 */
int app_analog_signal_calibrate_offsets(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_ANALOG_SIGNAL_H */
