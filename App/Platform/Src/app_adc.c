/*
 * App ADC - M1 采样链实现
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_adc.h"

#include "app_hrpwm.h"
#include "intf_gptmr.h"
#include "intf_hrpwm.h"
#include "intf_trgm.h"

#include <stddef.h>
#include <string.h>

extern void hpm_adc_driver_register(void);
extern void hpm_trgm_driver_register(void);
extern void hpm_hrpwm_driver_register(void);
extern void hpm_gptmr_driver_register(void);

/* ============================================================================
 * 通道映射（板级，与 pinmux.c 一致）
 * ============================================================================ */

typedef struct {
    uint8_t inst;  /* ADC 实例 */
    uint8_t hw_ch; /* 物理通道 */
} app_adc_map_t;

static const app_adc_map_t s_map[ADC_CH_COUNT] = {
    [ADC_CH_I_U] = {.inst = 0U, .hw_ch = 3U},    /* PB11 */
    [ADC_CH_I_V] = {.inst = 0U, .hw_ch = 4U},    /* PB12 */
    [ADC_CH_I_W] = {.inst = 0U, .hw_ch = 2U},    /* PB10 */
    [ADC_CH_V_VBUS] = {.inst = 1U, .hw_ch = 6U},  /* PB14 */
    [ADC_CH_NTC0] = {.inst = 1U, .hw_ch = 11U},   /* PB08（更正后原理图：PB08 = NTC0） */
    [ADC_CH_NTC1] = {.inst = 1U, .hw_ch = 1U},    /* PB09（更正后原理图：PB09 = NTC1） */
    [ADC_CH_V_CANID] = {.inst = 1U, .hw_ch = 15U}, /* PB00（ADC1 序列转换） */
};

/* PMT 队列：ADC0 = 三路电流（顺序转换，首槽 = 队尾副本） */
#define APP_ADC_CURRENT_COUNT (3U)
#define APP_ADC_PMT_TRIG_CH   (0U) /* ADC16_CONFIG_TRG0A（PWM1 CMP10 触发，仅 ADC0） */

/* 慢速通道（ADC1 序列转换 @1kHz：GPTMR0 CH2 → TRGM → ADC1_STRGI，硬件自主、无 ISR）：
 * 序列 [CANID, V_VBUS, NTC0, NTC1, CANID]——首项为队尾副本（首转换 S/H 残留自吸收），
 * 真实数据取 seq1..4；结果由硬件同步到 PRD_RESULTx，1kHz 慢任务纯寄存器读取。 */
#define APP_ADC_SLOW_CH_COUNT      (4U)
#define APP_ADC_SLOW_RATE_HZ       (1000U)
#define APP_ADC_SLOW_SEQ_COUNT     (5U)
#define APP_ADC_SLOW_TRIG_GPTMR_CH (2U) /* GPTMR0 CH2 → TRGM 输入 GPTMR0_OUT2 */
static const adc_channel_t s_slow_channels[APP_ADC_SLOW_CH_COUNT] = {
    ADC_CH_V_VBUS, ADC_CH_NTC0, ADC_CH_NTC1, ADC_CH_V_CANID,
};

/* ============================================================================
 * PMT 队列首槽行为与"双份队尾"方案（2026-09-19 实测确认）
 *
 *   硬件行为：PMT 队列的【首槽】会得到"上一帧最后一个通道"的采样值
 *             （首个转换的 S/H 尚未切到新通道），且该槽元数据仍为首通道号。
 *             为竞态：偶尔也出现首槽正确的情况。
 *   实测证据（交叉验证）：
 *     - 3 槽 [ch3,ch4,ch2]：slot0 读到 ch2 的值 → app 的 I_U 实为 I_W
 *       → "U/W 电流波形重合"
 *     - 3 槽 [ch6,ch1,ch11]：slot0 读到 ch11 的值（3.29V）
 *       → "V_VBUS 读数 73V（满量程）"
 *     - 4 槽 [ch3,ch4,ch2,ch6]：slot0 读到 ch6 的值（1.07V）
 *     - 4 槽 [ch6,ch1,ch11,ch3]：四槽全对（竞态的另一侧）
 *
 *   方案（不浪费通道）：把"会被污染的首槽"安排成【队尾通道的副本】——
 *     队列 = [D, A, B, D]，其中 D 同时位于首槽与队尾：
 *       - 若发生污染：slot0 = D 的上一帧值（元数据 D，自洽，仍有效）
 *       - 若未污染：  slot0 = D 的本帧值
 *       - slot3 = D 的本帧新鲜值（主用）
 *     真实数据一律取 slot1..3（本帧新鲜）；slot0 作为高调制时的备用副本
 *     （其采样点最早，始终落在低侧导通窗口内）。
 *   依据：手册 §53 的 PMT 数据格式（bit31 恒 1、bit30:29 转换序号、
 *         bit24:20 通道号）与本行为一致；该行为未收录于用户手册/勘误表。
 * ============================================================================ */
#define APP_ADC_SLOT_COUNT    (4U)  /* 3 真实通道 + 1 队尾副本（首槽） */

/* 实时链有效性掩码：仅三路相电流（PMT）。
 * 慢速通道（VBUS/NTC/CANID）按自己的节拍更新，不参与实时有效性判定。 */
#define APP_ADC_PMT_MASK ((1UL << APP_ADC_CURRENT_COUNT) - 1UL)

/* ============================================================================
 * State
 * ============================================================================ */

static volatile uint16_t s_raw[ADC_CH_COUNT];
static volatile uint32_t s_valid_mask;
static volatile uint32_t s_sequence;
static bool s_initialized;
static uint16_t s_full_scale_code = 65535U;
static app_adc_cfg_t s_cfg;

/* 驱动 WDOG 回调（硬件通道）→ 逻辑通道回调适配 */
static void adc_wdog_adapter(intf_adc_ch_t ch, uint16_t value, void *user) {
    uint8_t inst = INTF_ADC_CH_INST(ch);
    uint8_t hw = INTF_ADC_CH_IDX(ch);

    (void) user;
    for (uint8_t i = 0U; i < (uint8_t) ADC_CH_COUNT; i++) {
        if ((s_map[i].inst == inst) && (s_map[i].hw_ch == hw)) {
            if (s_cfg.wdog_cb != NULL) {
                s_cfg.wdog_cb((adc_channel_t) i, value, s_cfg.wdog_cb_user);
            }
            return;
        }
    }
}

/* 驱动 SEQ 完成回调 → 用户慢帧回调适配（1kHz，ISR） */
static void adc_slow_adapter(intf_adc_ch_t ch, void *user) {
    (void) ch;
    (void) user;
    if (s_cfg.slow_cb != NULL) {
        s_cfg.slow_cb();
    }
}

/* ============================================================================
 * ISR 回调（PMT 完成，队列顺序 = 通道枚举顺序）
 * ============================================================================ */

static void adc_current_pmt_cb(
    intf_adc_ch_t trig_ch, const uint16_t *values, uint8_t count, void *user_data) {
    (void) trig_ch;
    (void) user_data;

    /* 首槽为队尾副本（见上方双份队尾说明），真实数据从 values[1] 起 */
    for (uint8_t i = 0U; (i < APP_ADC_CURRENT_COUNT) && ((i + 1U) < count); i++) {
        uint8_t ch = (uint8_t) (ADC_CH_I_U + i);

        s_raw[ch] = values[i + 1U];
        s_valid_mask |= (1UL << ch);
    }
    s_sequence++;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void app_adc_init(const app_adc_cfg_t *cfg) {
    intf_adc_cfg_t a0;
    intf_adc_cfg_t a1;
    int rc0;
    int rc1 = 0;

    s_cfg = (app_adc_cfg_t) {
        .trigger_delay_ns = APP_ADC_TRIGGER_DELAY_NS_DEFAULT,
        .sample_cycle = APP_ADC_SAMPLE_CYCLE_DEFAULT,
        .resolution = (uint8_t) INTF_ADC_RES_DEFAULT,
    };
    if (cfg != NULL) {
        s_cfg = *cfg;
        if (s_cfg.sample_cycle == 0U) {
            s_cfg.sample_cycle = APP_ADC_SAMPLE_CYCLE_DEFAULT;
        }
        if (s_cfg.resolution == 0U) {
            s_cfg.resolution = (uint8_t) INTF_ADC_RES_DEFAULT;
        }
        if (s_cfg.trigger_delay_ns == 0U) {
            s_cfg.trigger_delay_ns = APP_ADC_TRIGGER_DELAY_NS_DEFAULT;
        }
    }

    s_full_scale_code = (uint16_t) ((1UL << s_cfg.resolution) - 1UL);

    hpm_adc_driver_register();
    hpm_trgm_driver_register();
    hpm_hrpwm_driver_register(); /* 触发比较器依赖 PWM 实例信息（幂等） */
    hpm_gptmr_driver_register(); /* 慢速触发源（幂等） */

    /* ---- ADC0：三路相电流（PMT 顺序转换） ---- */
    memset(&a0, 0, sizeof(a0));
    a0.resolution = (intf_adc_resolution_t) s_cfg.resolution;
    a0.mode = INTF_ADC_MODE_PMT;
    a0.sample_cycle = s_cfg.sample_cycle;
    a0.clock_div = INTF_ADC_DEFAULT_CLOCK_DIV;
    a0.vref_mv = INTF_ADC_DEFAULT_VREF_MV;
    a0.dma_en = true; /* DMA 缓冲由驱动内部分配（fast RAM） */
    a0.pmt_trig_ch = APP_ADC_PMT_TRIG_CH;
    a0.pmt_ch_count = APP_ADC_SLOT_COUNT;
    a0.pmt_cb = adc_current_pmt_cb;
    a0.wdog_en = s_cfg.wdog_en;
    a0.wdog_thshd_high = s_cfg.wdog_thshd_high;
    a0.wdog_thshd_low = s_cfg.wdog_thshd_low;
    a0.wdog_cb = adc_wdog_adapter;
    a0.wdog_cb_user_data = NULL;
    /* [I_W(副本), I_U, I_V, I_W]：首槽 = 队尾通道副本（见上方说明） */
    a0.pmt_ch_list[0] = s_map[ADC_CH_I_W].hw_ch;
    for (uint8_t i = 0U; i < APP_ADC_CURRENT_COUNT; i++) {
        a0.pmt_ch_list[i + 1U] = s_map[ADC_CH_I_U + i].hw_ch;
    }
    rc0 = intf_adc_init(INTF_ADC_CH(s_map[ADC_CH_I_U].inst, 0U), &a0);

    /* ---- ADC1：慢速通道 —— 序列转换 @1kHz（GPTMR0 CH2 → TRGM → ADC1_STRGI） ----
     * 与 25kHz PWM 触发完全解耦（不再占用 PTRGI0A/抢占队列），硬件自主、无 ISR。
     * 序列 [CANID, V_VBUS, NTC0, NTC1, CANID]：首项 = 队尾副本（首转换 S/H 残留
     * 自吸收），真实数据取 seq1..4；结果由硬件同步到 PRD_RESULTx（实测任意转换
     * 模式都会更新该寄存器 —— 手册 §53.2.4），1kHz 慢任务纯寄存器读取。 */
    memset(&a1, 0, sizeof(a1));
    a1.resolution = (intf_adc_resolution_t) s_cfg.resolution;
    a1.mode = INTF_ADC_MODE_SEQ;
    a1.sample_cycle = s_cfg.sample_cycle;
    a1.clock_div = INTF_ADC_DEFAULT_CLOCK_DIV;
    a1.vref_mv = INTF_ADC_DEFAULT_VREF_MV;
    a1.seq_hw_trig = true;
    a1.seq_ch_count = APP_ADC_SLOW_SEQ_COUNT;
    a1.seq_ch_list[0] = s_map[ADC_CH_V_CANID].hw_ch;
    a1.seq_ch_list[1] = s_map[ADC_CH_V_VBUS].hw_ch;
    a1.seq_ch_list[2] = s_map[ADC_CH_NTC0].hw_ch;
    a1.seq_ch_list[3] = s_map[ADC_CH_NTC1].hw_ch;
    a1.seq_ch_list[4] = s_map[ADC_CH_V_CANID].hw_ch;
    a1.seq_cb = (s_cfg.slow_cb != NULL) ? adc_slow_adapter : NULL;
    rc1 = intf_adc_init(INTF_ADC_CH(1U, 0U), &a1);

    /* ---- 慢速触发链：GPTMR0 CH2（1kHz 方波，50%）→ TRGM0 → ADC1_STRGI ---- */
    {
        intf_gptmr_cfg_t slow_trig;

        memset(&slow_trig, 0, sizeof(slow_trig));
        slow_trig.mode = INTF_GPTMR_MODE_PWM;
        slow_trig.frequency_hz = APP_ADC_SLOW_RATE_HZ;
        slow_trig.duty = 0.5f;
        rc1 |= intf_gptmr_init(APP_ADC_SLOW_TRIG_GPTMR_CH, &slow_trig);
        rc1 |= intf_gptmr_start(APP_ADC_SLOW_TRIG_GPTMR_CH);
    }
    (void) intf_trgm_connect(INTF_TRGM_SRC_GPTMR0_OUT2, INTF_TRGM_DST_ADC1_STRGI);

    /* ---- ADC0 触发链：PWM1 CMP10 → CH10REF → TRGM PTRGI0A ---- */
    (void) intf_trgm_connect(INTF_TRGM_SRC_PWM1_CH10REF, INTF_TRGM_DST_ADC_PTRGI0A);
    (void) intf_hrpwm_config_trigger_cmp(
        HRPWM_INST_1, APP_ADC_TRIGGER_CMP_INDEX, s_cfg.trigger_delay_ns);

    /* 启动 PWM1 计数器（仅计数，输出保持关闭）：PMT 触发依赖计数器运行，
     * 使采样链独立于逆变桥使能状态。 */
    (void) intf_hrpwm_start_counter_only(HRPWM_INST_1);

    s_initialized = (rc0 == 0) && (rc1 == 0);
}

const app_adc_cfg_t *app_adc_get_config(void) {
    return &s_cfg;
}

bool app_adc_get_raw(adc_channel_t ch, uint16_t *raw) {
    if ((ch >= ADC_CH_COUNT) || (raw == NULL) || !s_initialized) {
        return false;
    }
    if ((s_valid_mask & (1UL << ch)) == 0U) {
        return false;
    }

    *raw = s_raw[ch];
    return true;
}

uint32_t app_adc_get_sequence(void) {
    return s_sequence;
}

bool app_adc_is_valid(void) {
    /* 仅三路电流参与实时有效性判定（慢速通道按自己的节拍更新） */
    return s_initialized && ((s_valid_mask & APP_ADC_PMT_MASK) == APP_ADC_PMT_MASK);
}

float app_adc_code_to_volts(uint16_t code) {
    return ((float) code * (INTF_ADC_DEFAULT_VREF_MV / 1000.0f)) / (float) s_full_scale_code;
}

void app_adc_slow_process(void) {
    if (!s_initialized) {
        return;
    }

    /* ADC1 序列转换每 1ms 触发一轮（5 项），结果由硬件同步到 PRD_RESULTx；
     * 此处纯寄存器读取——无触发副作用、无读冲突。 */
    for (uint8_t i = 0U; i < APP_ADC_SLOW_CH_COUNT; i++) {
        adc_channel_t ch = s_slow_channels[i];
        intf_adc_ch_t enc = INTF_ADC_CH(s_map[ch].inst, s_map[ch].hw_ch);
        uint16_t raw = 0U;

        if (intf_adc_read(enc, &raw) != 0) {
            continue;
        }
        s_raw[ch] = raw;
        s_valid_mask |= (1UL << ch);
    }
}

int app_adc_set_trigger_delay_ns(uint32_t delay_ns) {
    int rc = intf_hrpwm_set_trigger_cmp_delay(
        HRPWM_INST_1, APP_ADC_TRIGGER_CMP_INDEX, delay_ns);

    if (rc == 0) {
        s_cfg.trigger_delay_ns = delay_ns;
    }
    return rc;
}

void app_adc_wdog_reenable(adc_channel_t ch) {
    if (ch >= ADC_CH_COUNT) {
        return;
    }
    intf_adc_wdog_reenable(INTF_ADC_CH(s_map[ch].inst, s_map[ch].hw_ch));
}
