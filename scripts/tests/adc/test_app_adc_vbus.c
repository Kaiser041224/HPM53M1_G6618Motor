/**
 * @file    test_app_adc_vbus.c
 * @brief   快通道母线（与电流同源触发的锁存 + 新鲜度握手）主机自测
 * @author  Kaiser
 *
 * 覆盖目标：
 *   ① ADC0 PMT ISR 每拍锁存 ADC1 SEQ 的 vbus 结果寄存器 → 电流环拿到"同拍"母线；
 *   ② SEQ 写指针（ADC16.SEQ_WR_ADDR）不推进时不采信结果寄存器、不推进序号，
 *      并提供陈旧计数 —— 保留 2026-09-22 summary #10 修复的"陈旧母线静默可用"缺口
 *      语义。ADC1 已于 2026-10-27 去掉周期中断（其帧计数不再推进），故新鲜度证据
 *      改由 SEQ 硬件的写指针推进提供。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_adc.h"
#include "intf_adc.h"
#include "test_util.h"

/* mock_deps.c */
extern intf_adc_cfg_t g_mock_adc_cfg[INTF_ADC_INSTANCE_COUNT];
extern uint16_t g_mock_prd_raw[INTF_ADC_INSTANCE_COUNT][16];
extern uint32_t g_mock_seq_progress[INTF_ADC_INSTANCE_COUNT];
extern void mock_adc_reset(void);
extern void mock_hardware_reset(void);
extern void mock_hrpwm_reset(void);
extern void mock_trgm_reset(void);

#define HW_VBUS (6U) /* ADC1_IN6 = PB14 */

/** @brief 触发一次 ADC0 PMT 完成回调（模拟驱动已校验过的 4 槽结果） */
static void pmt_frame(const intf_adc_cfg_t* cfg) {
    uint16_t values[4] = {0U, 3000U, 3001U, 3002U};

    cfg->pmt_cb(INTF_ADC_CH(0U, 0U), values, 4U, NULL);
}

void test_adc_vbus_fast_latch(void) {
    mock_adc_reset();
    mock_hardware_reset();
    mock_hrpwm_reset();
    mock_trgm_reset();
    app_adc_init(NULL);

    const intf_adc_cfg_t* cfg = &g_mock_adc_cfg[0];
    uint16_t raw = 0U;

    /* 首帧之前不可用（调用方须回退 1kHz 缓存） */
    CHECK(!app_adc_get_vbus_fast(&raw));
    CHECK(app_adc_get_vbus_fast_sequence() == 0U);

    /* 第 1 帧：ADC1 已完成 seq（帧计数 1）→ 锁存 */
    g_mock_prd_raw[1][HW_VBUS] = 12345U;
    pmt_frame(cfg);
    CHECK(app_adc_get_vbus_fast(&raw) && (raw == 12345U));
    CHECK(app_adc_get_vbus_fast_sequence() == 1U);
    CHECK(app_adc_get_vbus_fast_stale_count() == 0U);

    /* 第 2 帧：帧计数推进 + 新值 → 序号与值同步更新 */
    g_mock_prd_raw[1][HW_VBUS] = 12000U;
    pmt_frame(cfg);
    CHECK(app_adc_get_vbus_fast(&raw) && (raw == 12000U));
    CHECK(app_adc_get_vbus_fast_sequence() == 2U);
    CHECK(app_adc_get_vbus_fast_stale_count() == 0U);
}

void test_adc_vbus_fast_stale(void) {
    mock_adc_reset();
    mock_hardware_reset();
    mock_hrpwm_reset();
    mock_trgm_reset();
    app_adc_init(NULL);

    const intf_adc_cfg_t* cfg = &g_mock_adc_cfg[0];
    uint16_t raw = 0U;

    /* 建立首帧 */
    g_mock_prd_raw[1][HW_VBUS] = 1000U;
    pmt_frame(cfg);
    CHECK(app_adc_get_vbus_fast(&raw) && (raw == 1000U));
    CHECK(app_adc_get_vbus_fast_sequence() == 1U);

    /* 结果寄存器内容不可信（全 1）→ 不得被采信 */
    g_mock_prd_raw[1][HW_VBUS] = 0xFFFFU;
    pmt_frame(cfg);
    CHECK(app_adc_get_vbus_fast(&raw) && (raw == 1000U));
    CHECK(app_adc_get_vbus_fast_sequence() == 1U);
    CHECK(app_adc_get_vbus_fast_stale_count() == 1U);

    /* 恢复后陈旧计数停止增长，值更新 */
    g_mock_prd_raw[1][HW_VBUS] = 1010U;
    pmt_frame(cfg);
    CHECK(app_adc_get_vbus_fast(&raw) && (raw == 1010U));
    CHECK(app_adc_get_vbus_fast_sequence() == 2U);
    CHECK(app_adc_get_vbus_fast_stale_count() == 1U);
}