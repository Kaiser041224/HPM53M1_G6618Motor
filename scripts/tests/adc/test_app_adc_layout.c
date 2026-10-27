/**
 * @file    test_app_adc_layout.c
 * @brief   app_adc 采样链形态主机自测（队列形态 / 触发扇出 / 慢回调分频）
 * @author  Kaiser
 *
 * 覆盖目标（对应 2026-09-19 "PMT 首槽污染" 硬件行为的规避契约）：
 *   ① ADC0 PMT 队列首槽必须是队尾通道副本（否则复现 "V_VBUS 读数 73V"/"U/W 波形重合"）；
 *   ② 回调只取 slot1..3，首槽丢弃；
 *   ③ ADC1 SEQ 首末项同为 VBUS（自吸收 + 本帧新鲜值）；
 *   ④ PWM1_CH10REF 扇出到 ADC0 PTRGI0A 与 ADC1_STRGI，且不再经 GPTMR0；
 *   ⑤ 慢回调按 pwm_freq/1000 分频 → 30kHz 下 fault tick 仍为 1kHz（保护节拍不变）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_adc.h"
#include "app_hardware_params.h"
#include "intf_adc.h"
#include "intf_gptmr.h"
#include "intf_hrpwm.h"
#include "intf_trgm.h"
#include "test_util.h"

/* mock_deps.c */
extern intf_adc_cfg_t g_mock_adc_cfg[INTF_ADC_INSTANCE_COUNT];
extern int g_mock_adc_init_count[INTF_ADC_INSTANCE_COUNT];
extern int g_mock_trgm_count;
extern int g_mock_trigger_cmp_index;
extern int g_mock_trigger_cmp_calls;
extern int g_mock_counter_only_calls;
extern int g_mock_gptmr_init_calls;
extern int g_mock_gptmr_start_calls;
extern app_hardware_params_t* app_hardware_params_mutable(void);
extern void mock_adc_reset(void);
extern void mock_hardware_reset(void);
extern void mock_hrpwm_reset(void);
extern void mock_trgm_reset(void);
extern bool mock_trgm_has(intf_trgm_src_t src, intf_trgm_dst_t dst);

/* 硬件通道号（与 app_adc.c 映射表一致） */
#define HW_IU   (3U)
#define HW_IV   (4U)
#define HW_IW   (2U)
#define HW_VBUS (6U)
#define HW_NTC0 (11U)
#define HW_NTC1 (1U)
#define HW_CANID (15U)

static void adc_suite_init(void) {
    mock_adc_reset();
    mock_hardware_reset();
    mock_hrpwm_reset();
    mock_trgm_reset();
    app_adc_init(NULL);
}

/* ① ADC0 PMT 队列形态：首槽 == 队尾副本 */
void test_adc_pmt_queue_tail_duplicate(void) {
    adc_suite_init();

    const intf_adc_cfg_t* cfg = &g_mock_adc_cfg[0];

    CHECK(g_mock_adc_init_count[0] == 1);
    CHECK(cfg->mode == INTF_ADC_MODE_PMT);
    CHECK(cfg->pmt_ch_count == 4U);
    /* 首槽必须与队尾同通道：被污染时首槽内容仍属该通道，自洽有效 */
    CHECK(cfg->pmt_ch_list[0] == cfg->pmt_ch_list[cfg->pmt_ch_count - 1U]);
    /* 主用三路在 slot1..3 */
    CHECK(cfg->pmt_ch_list[1] == HW_IU);
    CHECK(cfg->pmt_ch_list[2] == HW_IV);
    CHECK(cfg->pmt_ch_list[3] == HW_IW);
    CHECK(cfg->pmt_cb != NULL);
}

/* ② 回调 slot 映射：只取 slot1..3，首槽值绝不进入电流缓存 */
void test_adc_pmt_slot_read_mapping(void) {
    adc_suite_init();

    const intf_adc_cfg_t* cfg = &g_mock_adc_cfg[0];
    uint16_t values[4] = {0xDEADU, 3000U, 3001U, 3002U};
    uint16_t raw = 0U;
    /* app_adc 的帧序号是全局单调计数器（不随 init 复位）→ 用相对增量断言，
     * 避免测试顺序依赖。 */
    uint32_t seq_before = app_adc_get_sequence();

    cfg->pmt_cb(INTF_ADC_CH(0U, 0U), values, 4U, NULL);

    CHECK(app_adc_get_raw(ADC_CH_I_U, &raw) && (raw == 3000U));
    CHECK(app_adc_get_raw(ADC_CH_I_V, &raw) && (raw == 3001U));
    CHECK(app_adc_get_raw(ADC_CH_I_W, &raw) && (raw == 3002U));
    /* 首槽（牺牲位）不得出现在任何电流通道 */
    CHECK(raw != 0xDEADU);
    CHECK(app_adc_get_sequence() == (seq_before + 1U));
    CHECK(app_adc_is_valid());
}

/* ③ ADC1 SEQ 序列形态：首末同为 VBUS */
void test_adc_seq_vbus_first_and_last(void) {
    adc_suite_init();

    const intf_adc_cfg_t* cfg = &g_mock_adc_cfg[1];

    CHECK(g_mock_adc_init_count[1] == 1);
    CHECK(cfg->mode == INTF_ADC_MODE_SEQ);
    CHECK(cfg->seq_hw_trig);
    CHECK(cfg->seq_ch_count == 5U);
    /* VBUS 首项：触发后最早完成，早于 ADC0 PMT，供同拍读取 */
    CHECK(cfg->seq_ch_list[0] == HW_VBUS);
    /* 首 == 末：首项被 S/H 残留污染时内容仍是 VBUS 自身 */
    CHECK(cfg->seq_ch_list[0] == cfg->seq_ch_list[cfg->seq_ch_count - 1U]);
    CHECK(cfg->seq_ch_list[1] == HW_NTC0);
    CHECK(cfg->seq_ch_list[2] == HW_NTC1);
    CHECK(cfg->seq_ch_list[3] == HW_CANID);
}

/* ④ 触发扇出：PWM1_CH10REF → ADC0 PTRGI0A 与 ADC1_STRGI，且不再经 GPTMR0 */
void test_adc_trigger_fanout(void) {
    adc_suite_init();

    CHECK(mock_trgm_has(INTF_TRGM_SRC_PWM1_CH10REF, INTF_TRGM_DST_ADC_PTRGI0A));
    CHECK(mock_trgm_has(INTF_TRGM_SRC_PWM1_CH10REF, INTF_TRGM_DST_ADC1_STRGI));
    /* GPTMR0 1kHz 自由相位链必须已移除（vbus 同步的前提） */
    CHECK(!mock_trgm_has(INTF_TRGM_SRC_GPTMR0_OUT2, INTF_TRGM_DST_ADC1_STRGI));
    CHECK(g_mock_trigger_cmp_calls == 1);
    CHECK(g_mock_trigger_cmp_index == (int)APP_ADC_TRIGGER_CMP_INDEX);
    /* PWM1 计数器仅计数启动（采样链独立于桥臂使能） */
    CHECK(g_mock_counter_only_calls == 1);
    /* 旧 GPTMR0 1kHz 自由相位触发链必须彻底不再初始化/启动 */
    CHECK(g_mock_gptmr_init_calls == 0);
    CHECK(g_mock_gptmr_start_calls == 0);
}

/* ⑤ 慢回调分频：30kHz 下每 30 帧一次 → fault tick 仍 1kHz */
static int s_slow_cb_calls;

static void slow_cb_counter(void) { s_slow_cb_calls++; }

void test_adc_slow_cb_decimation(void) {
    app_adc_cfg_t cfg;
    const intf_adc_cfg_t* adc1;
    int i;

    mock_adc_reset();
    mock_hardware_reset();
    mock_hrpwm_reset();
    mock_trgm_reset();
    app_hardware_params_mutable()->inverter.pwm_freq_hz = 30000U;

    cfg = (app_adc_cfg_t){0};
    cfg.sample_cycle = 10U;
    cfg.trigger_delay_ns = 500U;
    cfg.slow_cb = slow_cb_counter;
    app_adc_init(&cfg);

    adc1 = &g_mock_adc_cfg[1];
    CHECK(adc1->seq_cb != NULL);

    s_slow_cb_calls = 0;
    /* 分频比 = 30000/1000 = 30：前 29 帧不回调 */
    for (i = 0; i < 29; i++) {
        adc1->seq_cb(INTF_ADC_CH(1U, 0U), NULL);
    }
    CHECK(s_slow_cb_calls == 0);
    adc1->seq_cb(INTF_ADC_CH(1U, 0U), NULL);
    CHECK(s_slow_cb_calls == 1);
    /* 再 60 帧 = 2 次回调 */
    for (i = 0; i < 60; i++) {
        adc1->seq_cb(INTF_ADC_CH(1U, 0U), NULL);
    }
    CHECK(s_slow_cb_calls == 3);
}

/* ⑥ 分频退化为 1：PWM=1kHz 时逐帧回调（向后兼容） */
void test_adc_slow_cb_no_decimation(void) {
    app_adc_cfg_t cfg;
    const intf_adc_cfg_t* adc1;
    int i;

    mock_adc_reset();
    mock_hardware_reset();
    mock_hrpwm_reset();
    mock_trgm_reset();
    app_hardware_params_mutable()->inverter.pwm_freq_hz = 1000U;

    cfg = (app_adc_cfg_t){0};
    cfg.sample_cycle = 10U;
    cfg.trigger_delay_ns = 500U;
    cfg.slow_cb = slow_cb_counter;
    app_adc_init(&cfg);

    adc1 = &g_mock_adc_cfg[1];
    s_slow_cb_calls = 0;
    for (i = 0; i < 5; i++) {
        adc1->seq_cb(INTF_ADC_CH(1U, 0U), NULL);
    }
    CHECK(s_slow_cb_calls == 5);
}