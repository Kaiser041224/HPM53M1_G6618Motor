/**
 * @file    test_app_can_id.c
 * @brief   CAN 节点号（CANID 电阻网络）解码与 ADC 槽释放主机自测
 * @author  Kaiser
 *
 * 覆盖目标：
 *   ① 8 档实测原始码 → ID 0..7；阈值边界、开路/无数据拒绝；
 *   ② 上电采样：中值 + 离散度过滤，异常样本不参与解码；
 *   ③ CANID 槽释放：5 槽 → 4 槽（VBUS 首末不变），失败回退 5 槽；
 *   ④ 释放后慢速通道不再读取 CANID（valid 位清除）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_adc.h"
#include "app_can_id.h"
#include "intf_adc.h"
#include "test_util.h"

/* mock_deps.c */
extern intf_adc_cfg_t g_mock_adc_cfg[INTF_ADC_INSTANCE_COUNT];
extern uint16_t g_mock_prd_raw[INTF_ADC_INSTANCE_COUNT][16];
extern void mock_adc_reset(void);
extern void mock_adc_set_canid_sequence(const uint16_t* values, uint8_t count);
extern void mock_adc_set_status_on_delay(uint8_t inst, uint32_t flags);
extern void mock_hardware_reset(void);
extern void mock_hrpwm_reset(void);
extern void mock_trgm_reset(void);

/* 硬件通道号（与 app_adc.c 映射表一致） */
#define HW_VBUS  (6U)
#define HW_NTC0  (11U)
#define HW_NTC1  (1U)
#define HW_CANID (15U)

static void canid_suite_init(void) {
    mock_adc_reset();
    mock_hardware_reset();
    mock_hrpwm_reset();
    mock_trgm_reset();
    app_adc_init(NULL);
    g_mock_prd_raw[1][HW_VBUS] = 21400U;  /* 24 V 母线 */
    g_mock_prd_raw[1][HW_NTC0] = 57693U;  /* ~2.9 V */
    g_mock_prd_raw[1][HW_NTC1] = 65475U;  /* 开路 */
    g_mock_prd_raw[1][HW_CANID] = 508U;   /* DIP 000 */
}

/* ① 解码表：8 档实测值 → 拨码二进制 ID（S1 为 MSB）
 *
 * 电压档位与 ID 不同序：100（15 kΩ）→ raw≈11733 → ID 4，001（10 kΩ）→ ID 1。
 */
void test_can_id_decode_table(void) {
    static const uint16_t raws[8] = {508U,  11733U, 16295U, 23011U,
                                     33230U, 35292U, 37277U, 39697U};
    static const uint8_t ids[8] = {0U, 4U, 1U, 5U, 2U, 6U, 3U, 7U};

    for (uint8_t i = 0U; i < 8U; i++) {
        CHECK(app_can_id_decode(raws[i]) == ids[i]);
    }

    /* 阈值边界：>= 阈值进档；6119→000，6120→100(=4)；38486→011(=3)，38487→111(=7) */
    CHECK(app_can_id_decode(0U) == APP_CAN_ID_INVALID);
    CHECK(app_can_id_decode(100U) == APP_CAN_ID_INVALID);
    CHECK(app_can_id_decode(6119U) == 0U);
    CHECK(app_can_id_decode(6120U) == 4U);
    CHECK(app_can_id_decode(38486U) == 3U);
    CHECK(app_can_id_decode(38487U) == 7U);

    /* 全 1（开路/未写入）不得解成 ID 7 */
    CHECK(app_can_id_decode(65535U) == APP_CAN_ID_INVALID);
    CHECK(app_can_id_decode(65001U) == APP_CAN_ID_INVALID);
}

/* ② 上电采样：9 次取中值，异常值不参与 */
void test_can_id_read_samples_median(void) {
    canid_suite_init();
    g_mock_prd_raw[1][HW_CANID] = 23011U; /* 101 → ID 5 */

    CHECK(app_can_id_read() == 5U);
    CHECK(app_can_id_get_boot_raw() == 23011U);
    CHECK(app_can_id_get_boot_id() == 5U);
}

void test_can_id_read_rejects_no_data(void) {
    canid_suite_init();
    g_mock_prd_raw[1][HW_CANID] = 0U; /* ADC 未写入 */

    CHECK(app_can_id_read() == APP_CAN_ID_INVALID);
    CHECK(app_can_id_get_boot_raw() == 0xFFFFU);
}

/* ②' 上电瞬态离群样本：中值 + 内点过滤后仍应解出正确档位
 *
 * 台架现象（2026-11-03）：DIP 001 的三次启动中，一次中值 16323（正确，应为 id=1），
 * 但极差 >512 LSB 被判 invalid。boot 期间三相桥自检仍在 50% 开关，CANID 是
 * 无电容的高阻分压网络，上电前几帧可能出现离群值——不能一刀切否定整轮采样。
 */
void test_can_id_read_tolerates_outlier(void) {
    static const uint16_t sequence[9] = {16323U, 16323U, 30000U, 16323U, 16323U,
                                         16323U, 16323U, 16323U, 16323U};

    canid_suite_init();
    mock_adc_set_canid_sequence(sequence, 9U);

    CHECK(app_can_id_read() == 1U);
    CHECK(app_can_id_get_boot_raw() == 16323U);
    CHECK(app_can_id_get_boot_spread() <= 512U);
}

/* ③ 释放：4 槽重配成功 */
void test_can_id_release_success(void) {
    uint16_t raw = 0U;

    canid_suite_init();
    CHECK(app_can_id_read() == 0U);

    mock_adc_set_status_on_delay(1U, INTF_ADC_STATUS_SEQ_COMPLETE);
    CHECK(app_adc_canid_release() == 0);

    const intf_adc_cfg_t* cfg = &g_mock_adc_cfg[1];

    CHECK(cfg->seq_ch_count == 4U);
    CHECK(cfg->seq_hw_trig);
    /* VBUS 首末不变（快通道同拍读取与首槽自吸收的前提） */
    CHECK(cfg->seq_ch_list[0] == HW_VBUS);
    CHECK(cfg->seq_ch_list[1] == HW_NTC0);
    CHECK(cfg->seq_ch_list[2] == HW_NTC1);
    CHECK(cfg->seq_ch_list[3] == HW_VBUS);
    CHECK(app_adc_is_canid_released());
    /* CANID 不再作为有效通道暴露 */
    CHECK(!app_adc_get_raw(ADC_CH_V_CANID, &raw));
    /* 幂等：再次调用不触发重配 */
    CHECK(app_adc_canid_release() == 0);
    CHECK(g_mock_adc_cfg[1].seq_ch_count == 4U);
}

/* ③' 尚未产出 CANID 帧 → 保留 5 槽 */
void test_can_id_release_requires_first_frame(void) {
    canid_suite_init();
    /* 不调用 app_adc_slow_process()：CANID valid 位未置 */

    CHECK(app_adc_canid_release() == -1);
    CHECK(g_mock_adc_cfg[1].seq_ch_count == 5U);
    CHECK(!app_adc_is_canid_released());
}

/* ③'' 重配后无 SEQ 完成标志 → 回退 5 槽 */
void test_can_id_release_rolls_back_without_frame(void) {
    canid_suite_init();
    CHECK(app_can_id_read() == 0U);

    mock_adc_set_status_on_delay(1U, 0U); /* 模拟序列没有重新跑起来 */
    CHECK(app_adc_canid_release() == -1);
    CHECK(g_mock_adc_cfg[1].seq_ch_count == 5U);
    CHECK(g_mock_adc_cfg[1].seq_ch_list[3] == HW_CANID);
    CHECK(!app_adc_is_canid_released());
}
