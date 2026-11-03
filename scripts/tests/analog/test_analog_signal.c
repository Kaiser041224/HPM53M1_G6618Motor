/**
 * @file    test_analog_signal.c
 * @brief   模拟量层主机自测：快通道母线（换算口径 / 滤波采样率 / 路径隔离 / 不可用语义）
 * @author  Kaiser
 *
 * 覆盖目标：
 *   ① 快通道与 1kHz 路径**同换算口径**（v_per_volt），仅采样时刻/速率不同；
 *   ② 快通道 LPF 系数必须按**真实采样率 pwm_freq(30kHz)** 标定 —— 沿用 1kHz 会把
 *      α 从 0.295 抬到 0.926（2026-10-18 spec §4.7 已记录该坑：滤波器
 *      sample_rate_hz 与实际调用频率不一致）；
 *   ③ 快通道调用不得污染 1kHz 路径（监控/故障/辨识仍读 1kHz 缓存）；
 *   ④ 快通道无首帧时返回 NAN（调用方回退 1kHz 缓存）。
 *   ⑤ NTC 阻值 → 温度：B 参数法、有效区间、参数未配置拒绝换算；
 *   ⑥ 温度缓存：约 100 Hz 换算节拍（1 kHz 慢任务 / 10），允许陈旧上报。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_analog_signal.h"
#include "app_hardware_params.h"
#include "test_util.h"

#include <math.h>

/* mock_deps.c */
extern void mock_adc_reset(void);
extern void mock_hardware_reset(void);
extern void mock_set_slow_raw(adc_channel_t ch, uint16_t raw);
extern void mock_set_fast_vbus(uint16_t raw, uint32_t sequence);
extern void mock_set_fast_vbus_valid(bool valid);
extern app_hardware_params_t* app_hardware_params_mutable(void);

#define MOCK_VBUS_V_PER_VOLT (22.2121f)
#define MOCK_PWM_FREQ_HZ     (30000.0f)
#define MOCK_VBUS_FAST_FC_HZ (2000.0f)

#define TEST_TWO_PI_F (6.283185307f)

/**
 * @brief fast-math 安全的有限性判定（位模式检查，与 foc_finite 同法）
 *
 * 本套件 pass 2 用 -O2 -ffast-math 编译：该档隐含 -ffinite-math-only，
 * `isnan()/isfinite()` 会被常量折叠为 false → 断言静默失效。
 * 生产侧 `foc_finite()` 用 union 位与实现，不受影响；测试也必须同法。
 */
static bool test_finite(float x) {
    union {
        float f;
        uint32_t u;
    } pun = {.f = x};

    return (pun.u & 0x7F800000u) != 0x7F800000u;
}

/** @brief 期望的一阶 IIR 系数：α = Ts/(Ts+τ)，Ts = 1/f_s，τ = 1/(2π·fc) */
static float expected_alpha(float fc_hz, float fs_hz) {
    float ts = 1.0f / fs_hz;
    float tau = 1.0f / (TEST_TWO_PI_F * fc_hz);

    return ts / (ts + tau);
}

static void analog_suite_init(void) {
    mock_adc_reset();
    mock_hardware_reset();
    mock_set_fast_vbus_valid(false);
    app_analog_signal_init();
}

/* ① 换算口径：常量输入下 LPF 收敛值 == raw→volts→×v_per_volt */
void test_analog_vbus_fast_scale(void) {
    analog_suite_init();

    uint16_t raw = 32000U;
    float expected = app_adc_code_to_volts(raw) * MOCK_VBUS_V_PER_VOLT;
    float value = NAN;

    mock_set_fast_vbus(raw, 1U);
    for (uint32_t i = 1U; i <= 200U; i++) {
        mock_set_fast_vbus(raw, i); /* 每拍新序号 = 新帧 */
        value = app_analog_signal_vbus_fast();
    }

    CHECK(test_finite(value));
    CHECK_NEAR(value, expected, 1e-3f);
    /* 量级自检：32000/65535×3.3×22.2121 ≈ 35.8 V */
    CHECK(value > 35.0f && value < 36.0f);
}

/* ② 滤波采样率：α 必须按 30kHz 标定（1kHz 会把 α 抬到 0.926） */
void test_analog_vbus_fast_filter_rate(void) {
    analog_suite_init();

    uint16_t raw = 32000U;
    float y1, y2, alpha;

    mock_set_fast_vbus(raw, 1U);
    y1 = app_analog_signal_vbus_fast(); /* 首帧 priming：直接输出 */
    CHECK(test_finite(y1));
    CHECK(y1 > 0.0f);

    /* 输入置 0 → y2 = (1-α)·y1，故 α = 1 - y2/y1 */
    mock_set_fast_vbus(0U, 2U);
    y2 = app_analog_signal_vbus_fast();
    alpha = 1.0f - (y2 / y1);

    CHECK_NEAR(alpha, expected_alpha(MOCK_VBUS_FAST_FC_HZ, MOCK_PWM_FREQ_HZ), 0.02f);
    /* 判别性：与"错用 1kHz 采样率"的结果必须显著不同 */
    CHECK(fabsf(alpha - expected_alpha(MOCK_VBUS_FAST_FC_HZ, 1000.0f)) > 0.3f);
}

/* ③ 路径隔离：快通道调用不改变 1kHz 路径的物理量 */
void test_analog_vbus_fast_isolates_slow(void) {
    analog_suite_init();

    /* 1kHz 路径：由 app_analog_signal_process() 从慢通道 raw 计算 */
    mock_set_slow_raw(ADC_CH_V_VBUS, 16000U);
    app_analog_signal_process();
    float slow_before = app_analog_signal_read(ADC_CH_V_VBUS);
    CHECK(test_finite(slow_before));

    /* 快通道喂入完全不同的值 */
    mock_set_fast_vbus(60000U, 1U);
    float fast = app_analog_signal_vbus_fast();
    CHECK(test_finite(fast));
    CHECK(fast > slow_before);

    /* 1kHz 路径值不变 */
    CHECK_NEAR(app_analog_signal_read(ADC_CH_V_VBUS), slow_before, 1e-6f);

    /* 再跑一次 1kHz 换算（raw 未变）→ 值仍等于慢通道换成 16000 的结果 */
    app_analog_signal_process();
    CHECK_NEAR(app_analog_signal_read(ADC_CH_V_VBUS), slow_before, 1e-6f);
}

/* ④ 不可用语义：无首帧返回 NAN；同序号不重复推进滤波器 */
void test_analog_vbus_fast_unavailable(void) {
    analog_suite_init();

    /* 尚无快通道帧 → NAN（调用方回退 1kHz 缓存） */
    CHECK(!test_finite(app_analog_signal_vbus_fast()));

    /* 有帧但序号不推进 → 保持输出、不再灌同一样本 */
    mock_set_fast_vbus(32000U, 7U);
    float y1 = app_analog_signal_vbus_fast();
    CHECK(test_finite(y1));

    mock_set_fast_vbus(0U, 7U); /* 序号不变（陈旧） */
    float y2 = app_analog_signal_vbus_fast();
    CHECK_NEAR(y2, y1, 1e-6f); /* 未被 0 输入拖走 */
}

/* ⑤ NTC 温度换算：B 参数法 + 有效区间（100 Ω, 1 MΩ）
 *
 * 当前装配占位：R25 = 100 kΩ、B = 3950 K、上拉 = 10 kΩ。
 * 后续硬件会把上拉统一为 100 kΩ，MOS 与定子 NTC 同规格。 */
void test_analog_ntc_temp_from_ohm(void) {
    float t = 0.0f;

    /* R = R25 → 25 ℃ */
    CHECK(app_analog_signal_ntc_temp_from_ohm(100000.0f, 100000.0f, 3950.0f, &t));
    CHECK_NEAR(t, 25.0f, 0.2f);

    /* R = R25/10 → 1/(1/298.15 + ln(0.1)/3950) - 273.15 ≈ 87.72 ℃ */
    CHECK(app_analog_signal_ntc_temp_from_ohm(10000.0f, 100000.0f, 3950.0f, &t));
    CHECK_NEAR(t, 87.72f, 0.5f);

    /* 开路替代值（= max_ohm）与短路/越界 → 无效 */
    CHECK(!app_analog_signal_ntc_temp_from_ohm(1000000.0f, 100000.0f, 3950.0f, &t));
    CHECK(!app_analog_signal_ntc_temp_from_ohm(100.0f, 100000.0f, 3950.0f, &t));
    CHECK(!app_analog_signal_ntc_temp_from_ohm(0.0f, 100000.0f, 3950.0f, &t));

    /* 参数未配置（占位 0）→ 无效，不得除零 */
    CHECK(!app_analog_signal_ntc_temp_from_ohm(100000.0f, 0.0f, 3950.0f, &t));
    CHECK(!app_analog_signal_ntc_temp_from_ohm(100000.0f, 100000.0f, 0.0f, &t));

    /* 非有限输入：位模式构造 NaN（-ffast-math 下不能用 isnan） */
    union {
        float f;
        uint32_t u;
    } nan_in = {.u = 0x7FC00000U};

    CHECK(!app_analog_signal_ntc_temp_from_ohm(nan_in.f, 100000.0f, 3950.0f, &t));

    /* 输出指针可为 NULL：仅判有效 */
    CHECK(app_analog_signal_ntc_temp_from_ohm(100000.0f, 100000.0f, 3950.0f, NULL));
}

/* ⑥ 温度缓存：约 100 Hz 换算节拍（1 kHz 慢任务 / 10），允许陈旧上报 */
void test_analog_ntc_temp_cached(void) {
    analog_suite_init();

    float t = 0.0f;

    /* 10 kΩ 上拉 + 100 kΩ NTC：R = 100 kΩ → raw ≈ 59578（V ≈ 3.0 V） */
    mock_set_slow_raw(ADC_CH_NTC0, 59578U);
    mock_set_slow_raw(ADC_CH_NTC1, 65535U); /* 开路 → 无效 */
    app_analog_signal_process();

    CHECK(app_analog_signal_ntc_temp_c(ADC_CH_NTC0, &t));
    CHECK_NEAR(t, 25.0f, 0.5f);
    CHECK(!app_analog_signal_ntc_temp_c(ADC_CH_NTC1, &t));
    CHECK(!app_analog_signal_ntc_temp_c(ADC_CH_I_U, &t));
    CHECK(!app_analog_signal_ntc_temp_c(ADC_CH_NTC0, NULL));

    /* 100 Hz 节拍：其后 9 拍保持旧值，第 10 拍才更新 */
    mock_set_slow_raw(ADC_CH_NTC0, 32768U); /* R ≈ 10 kΩ → ≈ 87.7 ℃ */

    for (uint32_t i = 0U; i < 9U; i++) {
        app_analog_signal_process();
    }
    CHECK(app_analog_signal_ntc_temp_c(ADC_CH_NTC0, &t));
    CHECK_NEAR(t, 25.0f, 0.5f);

    app_analog_signal_process();
    CHECK(app_analog_signal_ntc_temp_c(ADC_CH_NTC0, &t));
    CHECK_NEAR(t, 87.72f, 1.0f);
}
