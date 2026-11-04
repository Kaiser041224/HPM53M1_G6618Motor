/**
 * @file    app_gptmr.c
 * @brief   GPTMR 外环定时器平台封装
 * @author  Kaiser
 *
 * GPTMR1 外环定时器封装。
 * 频率与相位需运行态验证，不声称与PWM锁相。
 * 通过 intf_gptmr 接口层访问驱动，不直接调用驱动层。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_gptmr.h"
#include "intf_gptmr.h"

#include <stddef.h>

/* GPTMR1 实例，全局通道号 4-7 */
#define APP_GPTMR_INSTANCE 1U
#define APP_GPTMR_CH_BASE  (APP_GPTMR_INSTANCE * 4U)

#define APP_GPTMR_CH0_FREQ 15000U
/* 预留 15kHz（motion 速度独立拍已合并至 CH0；motion timer 不启动本通道） */
#define APP_GPTMR_CH1_FREQ 15000U
#define APP_GPTMR_CH2_FREQ 10000U
/* 编码器采样：25kHz 的一半，供 25kHz FOC 复用（用时间戳算 dt） */
#define APP_GPTMR_CH3_FREQ 12500U /* 预留通道（原转子采样器频率，现无使用者） */

extern void hpm_gptmr_driver_register(void);

static app_gptmr_callback_t s_callbacks[APP_GPTMR_CH_COUNT];
static uint32_t s_channel_frequency[APP_GPTMR_CH_COUNT];

/**
 * @brief 将应用通道号转换为接口层全局通道号
 * @param ch 应用通道
 * @return 接口层通道
 */
static intf_gptmr_ch_t app_ch_to_intf(app_gptmr_ch_t ch) {
    return (intf_gptmr_ch_t)(APP_GPTMR_CH_BASE + (uint8_t)ch);
}

void app_gptmr_init(void) {
    hpm_gptmr_driver_register();
    s_channel_frequency[0] = APP_GPTMR_CH0_FREQ;
    s_channel_frequency[1] = APP_GPTMR_CH1_FREQ;
    s_channel_frequency[2] = APP_GPTMR_CH2_FREQ;
    s_channel_frequency[3] = APP_GPTMR_CH3_FREQ;

    /* CH0: motion 单控制拍 15kHz（position → velocity） */
    intf_gptmr_cfg_t cfg_voltage = {
        .mode = INTF_GPTMR_MODE_TIMER,
        .frequency_hz = APP_GPTMR_CH0_FREQ,
        .callback = NULL,
        .enable_sync = false,
    };
    (void)intf_gptmr_init(app_ch_to_intf(APP_GPTMR_CH_0), &cfg_voltage);

    /* CH1: 预留 15kHz（motion 速度独立拍已合并至 CH0，motion timer 不启动） */
    intf_gptmr_cfg_t cfg_power = {
        .mode = INTF_GPTMR_MODE_TIMER,
        .frequency_hz = APP_GPTMR_CH1_FREQ,
        .callback = NULL,
        .enable_sync = false,
    };
    (void)intf_gptmr_init(app_ch_to_intf(APP_GPTMR_CH_1), &cfg_power);

    /* CH2: 通用第三通道 10kHz */
    intf_gptmr_cfg_t cfg_ch2 = {
        .mode = INTF_GPTMR_MODE_TIMER,
        .frequency_hz = APP_GPTMR_CH2_FREQ,
        .callback = NULL,
        .enable_sync = false,
    };
    (void)intf_gptmr_init(app_ch_to_intf(APP_GPTMR_CH_2), &cfg_ch2);

    /* CH3: 预留（原 12.5kHz 编码器采样器；转子已改为 ADC 同拍硬件同步采样） */
    intf_gptmr_cfg_t cfg_ch3 = {
        .mode = INTF_GPTMR_MODE_TIMER,
        .frequency_hz = APP_GPTMR_CH3_FREQ,
        .callback = NULL,
        .enable_sync = false,
    };
    (void)intf_gptmr_init(app_ch_to_intf(APP_GPTMR_CH_3), &cfg_ch3);

    s_callbacks[APP_GPTMR_CH_0] = NULL;
    s_callbacks[APP_GPTMR_CH_1] = NULL;
    s_callbacks[APP_GPTMR_CH_2] = NULL;
    s_callbacks[APP_GPTMR_CH_3] = NULL;
}

int app_gptmr_register_callback(app_gptmr_ch_t ch, app_gptmr_callback_t cb) {
    if (ch >= APP_GPTMR_CH_COUNT) {
        return -1;
    }

    intf_gptmr_cfg_t cfg = {
        .mode = INTF_GPTMR_MODE_TIMER,
        .frequency_hz = s_channel_frequency[ch],
        .callback = cb,
        .enable_sync = false,
    };
    int rc = intf_gptmr_init(app_ch_to_intf(ch), &cfg);
    if (rc == 0)
        s_callbacks[ch] = cb;
    return rc;
}

int app_gptmr_start(app_gptmr_ch_t ch) {
    if (ch >= APP_GPTMR_CH_COUNT) {
        return -1;
    }
    return intf_gptmr_start(app_ch_to_intf(ch));
}

int app_gptmr_stop(app_gptmr_ch_t ch) {
    if (ch >= APP_GPTMR_CH_COUNT) {
        return -1;
    }
    return intf_gptmr_stop(app_ch_to_intf(ch));
}

void app_gptmr_start_all(void) {
    for (app_gptmr_ch_t ch = APP_GPTMR_CH_0; ch < APP_GPTMR_CH_COUNT; ch++) {
        (void)app_gptmr_start(ch);
    }
}

void app_gptmr_stop_all(void) {
    for (app_gptmr_ch_t ch = APP_GPTMR_CH_0; ch < APP_GPTMR_CH_COUNT; ch++) {
        (void)app_gptmr_stop(ch);
    }
}

int app_gptmr_set_frequency(app_gptmr_ch_t ch, uint32_t frequency_hz) {
    if (ch >= APP_GPTMR_CH_COUNT) {
        return -1;
    }
    if (frequency_hz == 0)
        return -1;
    int rc = intf_gptmr_set_frequency(app_ch_to_intf(ch), frequency_hz);
    if (rc == 0)
        s_channel_frequency[ch] = frequency_hz;
    return rc;
}
