/**
 * @file    app_analog_signal.c
 * @brief   物理量换算实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_analog_signal.h"

#include "algo_filter.h"
#include "app_hardware_params.h"
#include "intf_clock.h"

#include <math.h>
#include <stddef.h>

/* ============================================================================
 * 硬件换算参数（来源 config/hardware.yaml，init 时加载）
 * ============================================================================ */

static app_hardware_params_t s_hardware_params;

/* 零点标定过程参数（不随 YAML，标定流程专用） */
#define APP_ANALOG_ZERO_CAL_FRAMES      (256U)   /* ~10ms @25kHz（TBD：随 pwm_freq 派生） */
#define APP_ANALOG_ZERO_CAL_TIMEOUT_US  (50000U) /* 50ms 超时 */
#define APP_ANALOG_ZERO_CAL_MAX_DEV_PCT (5U)     /* 偏离中值超过 5% FS → 视为有电流 */

/* ============================================================================
 * 滤波器（NONE / MA / LPF）
 * ============================================================================ */

typedef enum {
    APP_ANALOG_FILTER_NONE = 0,
    APP_ANALOG_FILTER_MA,
    APP_ANALOG_FILTER_LPF,
} app_analog_filter_type_t;

typedef struct {
    app_analog_filter_type_t type;
    uint16_t ma_window;  /* type = MA */
    float* ma_buffer;    /* type = MA */
    float lpf_cutoff_hz; /* type = LPF */
    algo_ma_t ma;
    algo_lpf_t lpf;
} app_analog_filter_t;

static app_analog_filter_t s_filters[ADC_CH_COUNT] = {
    [ADC_CH_I_U] = {.type = APP_ANALOG_FILTER_NONE},
    [ADC_CH_I_V] = {.type = APP_ANALOG_FILTER_NONE},
    [ADC_CH_I_W] = {.type = APP_ANALOG_FILTER_NONE},
    /* 慢速通道（ADC1 读取模式 @1kHz）：值本身为直流，process 以主循环节拍（=
     * inverter.pwm_freq_hz，config/hardware.yaml）重复调用，
     * 滤波系数与调用频率不匹配会过度滤波，故暂用直通；待 FOC 阶段按 1kHz 重新标定。 */
    [ADC_CH_V_VBUS] = {.type = APP_ANALOG_FILTER_NONE},
    [ADC_CH_NTC0] = {.type = APP_ANALOG_FILTER_NONE},
    [ADC_CH_NTC1] = {.type = APP_ANALOG_FILTER_NONE},
    [ADC_CH_V_CANID] = {.type = APP_ANALOG_FILTER_NONE},
};

/* ============================================================================
 * State
 * ============================================================================ */

#define APP_ANALOG_CURRENT_COUNT (3U)

static float s_zero_volts[APP_ANALOG_CURRENT_COUNT];
static float s_phys_values[ADC_CH_COUNT];
static bool s_valid;

/* ============================================================================
 * 换算
 * ============================================================================ */

/**
 * @brief 由 NTC 分压电压推算热敏电阻阻值
 * @param volts 分压点电压
 * @return NTC 阻值 [Ω]（越界时返回替代值）
 */
static float ntc_resistance_from_volts(float volts) {
    const float vref_volts = INTF_ADC_DEFAULT_VREF_MV / 1000.0f;

    if (volts <= 0.0f) {
        return 0.0f;
    }
    if (volts >= (vref_volts - 0.001f)) { /* 悬空/超量程 */
        return s_hardware_params.ntc.max_ohm;
    }
    return s_hardware_params.ntc.pullup_ohm * volts / (vref_volts - volts);
}

/**
 * @brief 将 ADC 原始码按通道类型换算为物理量
 * @param ch 逻辑通道
 * @param raw ADC 原始码
 * @return 物理量（电流 [A] / 电压 [V] / 阻值 [Ω]）
 */
static float channel_to_physical(adc_channel_t ch, uint16_t raw) {
    float volts = app_adc_code_to_volts(raw);

    switch (ch) {
    case ADC_CH_I_U:
    case ADC_CH_I_V:
    case ADC_CH_I_W: return (volts - s_zero_volts[ch]) * s_hardware_params.current_sense.a_per_volt;
    case ADC_CH_V_VBUS: return volts * s_hardware_params.vbus_sense.v_per_volt;
    case ADC_CH_NTC0:
    case ADC_CH_NTC1: return ntc_resistance_from_volts(volts);
    case ADC_CH_V_CANID: return volts; /* CANID DIP 电阻网络电压（解码表待补充） */
    default: return 0.0f;
    }
}

/**
 * @brief 对输入样本执行该通道配置的滤波
 * @param ch 逻辑通道
 * @param x 输入样本
 * @return 滤波后样本
 */
static float filter_step(adc_channel_t ch, float x) {
    app_analog_filter_t* filter = &s_filters[ch];

    switch (filter->type) {
    case APP_ANALOG_FILTER_MA: return algo_ma_step_fast(&filter->ma, x);
    case APP_ANALOG_FILTER_LPF: return algo_lpf_step_fast(&filter->lpf, x);
    case APP_ANALOG_FILTER_NONE:
    default: return x;
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void app_analog_signal_init(void) {
    app_hardware_params_load(&s_hardware_params); /* config/hardware.yaml（将来 flash 覆盖） */

    for (uint8_t i = 0U; i < APP_ANALOG_CURRENT_COUNT; i++) {
        s_zero_volts[i] = s_hardware_params.current_sense.bias_v;
    }
    for (uint8_t ch = 0U; ch < ADC_CH_COUNT; ch++) {
        s_phys_values[ch] = 0.0f;
    }

    for (uint8_t ch = 0U; ch < ADC_CH_COUNT; ch++) {
        app_analog_filter_t* filter = &s_filters[ch];

        if (filter->type == APP_ANALOG_FILTER_MA) {
            algo_ma_cfg_t cfg = {
                .window_size = filter->ma_window,
                .buffer = filter->ma_buffer,
            };
            algo_ma_ctor(&filter->ma);
            (void)filter->ma.init(&filter->ma, &cfg);
        } else if (filter->type == APP_ANALOG_FILTER_LPF) {
            algo_lpf_cfg_t cfg = {
                .cutoff_hz = filter->lpf_cutoff_hz,
                .sample_rate_hz = (float)s_hardware_params.inverter.pwm_freq_hz,
            };
            algo_lpf_ctor(&filter->lpf);
            (void)filter->lpf.init(&filter->lpf, &cfg);
        } else {
            /* 无滤波 */
        }
    }

    s_valid = false;
}

void app_analog_signal_process(void) {
    for (uint8_t ch = 0U; ch < ADC_CH_COUNT; ch++) {
        uint16_t raw;

        if (!app_adc_get_raw((adc_channel_t)ch, &raw)) {
            continue;
        }
        s_phys_values[ch] =
            filter_step((adc_channel_t)ch, channel_to_physical((adc_channel_t)ch, raw));
    }

    if (app_adc_is_valid()) {
        s_valid = true;
    }
}

bool app_analog_signal_read_all(app_analog_values_t* values) {
    if ((values == NULL) || !s_valid) {
        return false;
    }

    values->i_u_a = s_phys_values[ADC_CH_I_U];
    values->i_v_a = s_phys_values[ADC_CH_I_V];
    values->i_w_a = s_phys_values[ADC_CH_I_W];
    values->v_bus_v = s_phys_values[ADC_CH_V_VBUS];
    values->r_ntc0_ohm = s_phys_values[ADC_CH_NTC0];
    values->r_ntc1_ohm = s_phys_values[ADC_CH_NTC1];
    return true;
}

float app_analog_signal_read(adc_channel_t ch) {
    if ((ch >= ADC_CH_COUNT) || !s_valid) {
        return NAN;
    }
    return s_phys_values[ch];
}

bool app_analog_signal_read_raw(adc_channel_t ch, uint16_t* raw) {
    return app_adc_get_raw(ch, raw);
}

int app_analog_signal_calibrate_offsets(void) {
    uint32_t full_scale;
    uint32_t mid_code;
    uint32_t max_dev;
    uint32_t start_seq;
    uint32_t frames_acquired = 0U;
    uint64_t raw_sum[APP_ANALOG_CURRENT_COUNT] = {0};
    uint32_t start_cycle;
    uint32_t timeout_cycles;
    uint8_t resolution;

    resolution = app_adc_get_config()->resolution;
    full_scale = (1UL << resolution) - 1UL;
    mid_code = full_scale / 2U;
    max_dev = full_scale * APP_ANALOG_ZERO_CAL_MAX_DEV_PCT / 100U;

    start_cycle = intf_clock_get_cycle();
    timeout_cycles = (intf_clock_get_cpu_freq() / 1000000U) * APP_ANALOG_ZERO_CAL_TIMEOUT_US;

    /* 等待链路就绪（首帧到达 + 启动丢弃期结束） */
    while (!app_adc_is_valid()) {
        if ((intf_clock_get_cycle() - start_cycle) > timeout_cycles) {
            return -1;
        }
    }

    start_seq = app_adc_get_sequence();

    while (frames_acquired < APP_ANALOG_ZERO_CAL_FRAMES) {
        uint32_t seq = app_adc_get_sequence();

        if (seq != start_seq) {
            for (uint8_t i = 0U; i < APP_ANALOG_CURRENT_COUNT; i++) {
                uint16_t raw;

                if (!app_adc_get_raw((adc_channel_t)(ADC_CH_I_U + i), &raw)) {
                    return -1;
                }
                /* 偏离中值过大 → 判定为有电流，拒绝标定 */
                if ((raw > (mid_code + max_dev)) || (raw < (mid_code - max_dev))) {
                    return -1;
                }
                raw_sum[i] += raw;
            }
            frames_acquired++;
            start_seq = seq;
        }

        if ((intf_clock_get_cycle() - start_cycle) > timeout_cycles) {
            return -1;
        }
    }

    for (uint8_t i = 0U; i < APP_ANALOG_CURRENT_COUNT; i++) {
        uint16_t average = (uint16_t)(raw_sum[i] / APP_ANALOG_ZERO_CAL_FRAMES);

        s_zero_volts[i] = app_adc_code_to_volts(average);
    }
    return 0;
}
