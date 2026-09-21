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
 * 硬件换算参数（来源 config/hardware.yaml）
 *   换算因子（a_per_volt / v_per_volt / NTC）逐次使用读取 → 支持 Shell 在线调整；
 *   零点偏置（bias_v）在 init 时快照到 s_zero_volts。
 * ============================================================================ */

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
        return app_hardware_params_current()->ntc.max_ohm;
    }
    return app_hardware_params_current()->ntc.pullup_ohm * volts / (vref_volts - volts);
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
    case ADC_CH_I_W:
        return (volts - s_zero_volts[ch]) * app_hardware_params_current()->current_sense.a_per_volt;
    case ADC_CH_V_VBUS: return volts * app_hardware_params_current()->vbus_sense.v_per_volt;
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
    for (uint8_t i = 0U; i < APP_ANALOG_CURRENT_COUNT; i++) {
        s_zero_volts[i] = app_hardware_params_current()->current_sense.bias_v;
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
                .sample_rate_hz = (float)app_hardware_params_current()->inverter.pwm_freq_hz,
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

/* ============================================================================
 * 电流零点标定（非阻塞状态机；阻塞包装供 boot 使用）
 * ============================================================================ */

#define APP_ANALOG_ZERO_CAL_FRAMES_PER_STEP (32U)  /* 单次 step 最多累积帧数 */
#define APP_ANALOG_ZERO_CAL_STEP_BUDGET_US  (200U) /* 单次 step 时间预算 */

/** 标定状态（非阻塞） */
typedef struct {
    bool active;                                /**< 进行中 */
    uint32_t start_cycle;                       /**< 起始 cycle（超时基准） */
    uint32_t timeout_cycles;                    /**< 超时周期数 */
    uint32_t start_seq;                         /**< 上一帧序列号 */
    uint32_t frames_acquired;                   /**< 已累积帧数 */
    uint32_t mid_code;                          /**< 中值码 */
    uint32_t max_dev;                           /**< 允许偏离 */
    uint64_t raw_sum[APP_ANALOG_CURRENT_COUNT]; /**< 原始码累加 */
} app_analog_calib_t;

static app_analog_calib_t s_calib;

int app_analog_signal_calibrate_start(void) {
    uint32_t full_scale;
    uint8_t resolution;

    resolution = app_adc_get_config()->resolution;
    full_scale = (1UL << resolution) - 1UL;

    s_calib.active = true;
    s_calib.start_cycle = intf_clock_get_cycle();
    s_calib.timeout_cycles =
        (intf_clock_get_cpu_freq() / 1000000U) * APP_ANALOG_ZERO_CAL_TIMEOUT_US;
    s_calib.start_seq = app_adc_get_sequence();
    s_calib.frames_acquired = 0U;
    s_calib.mid_code = full_scale / 2U;
    s_calib.max_dev = full_scale * APP_ANALOG_ZERO_CAL_MAX_DEV_PCT / 100U;
    for (uint8_t i = 0U; i < APP_ANALOG_CURRENT_COUNT; i++) {
        s_calib.raw_sum[i] = 0U;
    }

    return 0;
}

int app_analog_signal_calibrate_step(void) {
    uint32_t step_start;
    uint32_t frames_this_step = 0U;

    if (!s_calib.active) {
        return -1;
    }

    /* 等待链路就绪（首帧到达 + 启动丢弃期结束） */
    if (!app_adc_is_valid()) {
        if ((intf_clock_get_cycle() - s_calib.start_cycle) > s_calib.timeout_cycles) {
            s_calib.active = false;
            return -1;
        }
        return 1;
    }

    step_start = intf_clock_get_cycle();
    while (s_calib.frames_acquired < APP_ANALOG_ZERO_CAL_FRAMES) {
        uint32_t seq = app_adc_get_sequence();

        if (seq != s_calib.start_seq) {
            for (uint8_t i = 0U; i < APP_ANALOG_CURRENT_COUNT; i++) {
                uint16_t raw;

                if (!app_adc_get_raw((adc_channel_t)(ADC_CH_I_U + i), &raw)) {
                    s_calib.active = false;
                    return -1;
                }
                /* 偏离中值过大 → 判定为有电流，拒绝标定 */
                if ((raw > (s_calib.mid_code + s_calib.max_dev))
                    || (raw < (s_calib.mid_code - s_calib.max_dev))) {
                    s_calib.active = false;
                    return -1;
                }
                s_calib.raw_sum[i] += raw;
            }
            s_calib.frames_acquired++;
            s_calib.start_seq = seq;
            frames_this_step++;
        }

        if ((intf_clock_get_cycle() - s_calib.start_cycle) > s_calib.timeout_cycles) {
            s_calib.active = false;
            return -1;
        }
        if (frames_this_step >= APP_ANALOG_ZERO_CAL_FRAMES_PER_STEP) {
            break; /* 单次调用有界 */
        }
        if ((intf_clock_get_cycle() - step_start)
            > (intf_clock_get_cpu_freq() / 1000000U) * APP_ANALOG_ZERO_CAL_STEP_BUDGET_US) {
            break;
        }
    }

    if (s_calib.frames_acquired < APP_ANALOG_ZERO_CAL_FRAMES) {
        return 1;
    }

    for (uint8_t i = 0U; i < APP_ANALOG_CURRENT_COUNT; i++) {
        uint16_t average = (uint16_t)(s_calib.raw_sum[i] / APP_ANALOG_ZERO_CAL_FRAMES);

        s_zero_volts[i] = app_adc_code_to_volts(average);
    }
    s_calib.active = false;

    return 0;
}

void app_analog_signal_calibrate_cancel(void) {
    s_calib.active = false;
}

int app_analog_signal_calibrate_offsets(void) {
    int rc = app_analog_signal_calibrate_start();

    if (rc != 0) {
        return -1;
    }
    while ((rc = app_analog_signal_calibrate_step()) == 1) {
        /* 阻塞推进（boot 阶段；运行期请用 job 驱动 start/step） */
    }

    return rc;
}
