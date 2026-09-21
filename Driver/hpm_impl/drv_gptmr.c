/**
 * @file    drv_gptmr.c
 * @brief   GPTMR 驱动 - 通用定时器实现（PWM/定时中断/输入捕获）
 * @author  Kaiser
 *
 * Supports GPTMR0~3, per-channel modes:
 *   PWM output, timer interrupt, PWM+interrupt, input capture.
 *   SYNCI support for hardware synchronization via SYNT + TRGM.
 *
 * Channel encoding: ch = inst * 4 + local_ch
 *   0-3   = GPTMR0 CH0-CH3
 *   4-7   = GPTMR1 CH0-CH3
 *   8-11  = GPTMR2 CH0-CH3
 *   12-15 = GPTMR3 CH0-CH3
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_gptmr.h"

#include "hpm_gptmr_drv.h"
#include "hpm_clock_drv.h"
#include "hpm_common.h"
#include "hpm_interrupt.h"
#include "hpm_soc.h"
#include "hpm_soc_irq.h"

#include <stddef.h>

#define GPTMR_INSTANCE_COUNT    4U
#define GPTMR_CHANNELS_PER_INST 4U
#define GPTMR_TOTAL_CHANNELS    (GPTMR_INSTANCE_COUNT * GPTMR_CHANNELS_PER_INST)

/**
 * @brief GPTMR 通道运行状态
 */
typedef struct {
    bool configured;                    /**< 是否已配置 */
    intf_gptmr_mode_t mode;             /**< 工作模式 */
    uint32_t frequency_hz;              /**< 频率 [Hz] */
    float duty;                         /**< 占空比 [0.0-1.0] */
    bool invert_output;                 /**< 输出反相 */
    uint32_t reload;                    /**< 重载值 */
    intf_gptmr_irq_callback_t callback; /**< 定时中断回调 */
} gptmr_ch_state_t;

static GPTMR_Type * const s_gptmr_bases[GPTMR_INSTANCE_COUNT] = {
    HPM_GPTMR0,
    HPM_GPTMR1,
    HPM_GPTMR2,
    HPM_GPTMR3,
};

static const clock_name_t s_gptmr_clocks[GPTMR_INSTANCE_COUNT] = {
    clock_gptmr0,
    clock_gptmr1,
    clock_gptmr2,
    clock_gptmr3,
};

static const uint32_t s_gptmr_irq_nums[GPTMR_INSTANCE_COUNT] = {
    IRQn_GPTMR0,
    IRQn_GPTMR1,
    IRQn_GPTMR2,
    IRQn_GPTMR3,
};

ATTR_PLACE_AT_FAST_RAM_BSS static gptmr_ch_state_t s_gptmr_state[GPTMR_TOTAL_CHANNELS];

/**
 * @brief 校验通道号是否有效
 * @param ch 通道号
 * @return true = 有效
 */
static bool gptmr_ch_is_valid(intf_gptmr_ch_t ch)
{
    return ch < GPTMR_TOTAL_CHANNELS;
}

/**
 * @brief 由通道号取实例号
 * @param ch 通道号
 * @return 实例号
 */
static uint8_t gptmr_inst_from_ch(intf_gptmr_ch_t ch)
{
    return ch / GPTMR_CHANNELS_PER_INST;
}

/**
 * @brief 由通道号取实例内本地通道
 * @param ch 通道号
 * @return 本地通道号
 */
static uint8_t gptmr_local_ch(intf_gptmr_ch_t ch)
{
    return ch % GPTMR_CHANNELS_PER_INST;
}

/**
 * @brief 获取通道所属 GPTMR 基地址
 * @param ch 通道号
 * @return GPTMR 基地址
 */
static GPTMR_Type *gptmr_get_base(intf_gptmr_ch_t ch)
{
    return s_gptmr_bases[gptmr_inst_from_ch(ch)];
}

/**
 * @brief 校验占空比是否合法
 * @param duty 占空比
 * @return true = 合法（非 NaN 且 [0,1]）
 */
static bool gptmr_is_valid_duty(float duty)
{
    return (duty == duty) && (duty >= 0.0f) && (duty <= 1.0f);
}

/**
 * @brief 应用占空比（更新 CMP0/CMP1）
 * @param ch 通道号
 * @param duty 占空比
 * @return 0 = 成功；-1 = 占空比非法
 */
static int gptmr_apply_duty(intf_gptmr_ch_t ch, float duty)
{
    uint8_t lch = gptmr_local_ch(ch);
    GPTMR_Type *base = gptmr_get_base(ch);

    if (!gptmr_is_valid_duty(duty)) {
        return -1;
    }

    uint32_t cmp = (uint32_t)((float)s_gptmr_state[ch].reload * duty);
    s_gptmr_state[ch].duty = duty;

    gptmr_update_cmp(base, lch, 0, cmp);
    gptmr_update_cmp(base, lch, 1, s_gptmr_state[ch].reload);
    return 0;
}

/**
 * @brief 捕获边沿映射为 SDK 工作模式
 * @param edge 捕获边沿
 * @return SDK 工作模式
 */
static gptmr_work_mode_t gptmr_get_capture_mode(intf_gptmr_capture_edge_t edge)
{
    switch (edge) {
    case INTF_GPTMR_CAPTURE_EDGE_RISING:
        return gptmr_work_mode_capture_at_rising_edge;
    case INTF_GPTMR_CAPTURE_EDGE_FALLING:
        return gptmr_work_mode_capture_at_falling_edge;
    case INTF_GPTMR_CAPTURE_EDGE_BOTH:
        return gptmr_work_mode_capture_at_both_edge;
    default:
        return gptmr_work_mode_capture_at_rising_edge;
    }
}

/**
 * @brief 捕获边沿映射为 SDK 计数类型
 * @param edge 捕获边沿
 * @return SDK 计数类型
 */
static gptmr_counter_type_t gptmr_get_capture_counter_type(intf_gptmr_capture_edge_t edge)
{
    switch (edge) {
    case INTF_GPTMR_CAPTURE_EDGE_FALLING:
        return gptmr_counter_type_falling_edge;
    case INTF_GPTMR_CAPTURE_EDGE_RISING:
    case INTF_GPTMR_CAPTURE_EDGE_BOTH:
    default:
        return gptmr_counter_type_rising_edge;
    }
}

/**
 * @brief 计算两次捕获计数差（处理计数回绕）
 * @param first 前一次计数
 * @param next 后一次计数
 * @return 差值
 */
static uint32_t gptmr_calc_delta(uint32_t first, uint32_t next)
{
    return (next >= first) ? (next - first) : ((UINT32_MAX - first) + next + 1U);
}

/**
 * @brief 实例定时中断派发：清标志并触发对应通道回调
 * @param inst 实例号
 */
static void gptmr_isr_dispatch_instance(uint8_t inst)
{
    GPTMR_Type *base = s_gptmr_bases[inst];
    uint8_t ch_base = inst * GPTMR_CHANNELS_PER_INST;

    for (uint8_t i = 0; i < GPTMR_CHANNELS_PER_INST; i++) {
        intf_gptmr_ch_t ch = ch_base + i;
        if (!s_gptmr_state[ch].configured) {
            continue;
        }
        if (s_gptmr_state[ch].mode != INTF_GPTMR_MODE_TIMER
            && s_gptmr_state[ch].mode != INTF_GPTMR_MODE_PWM_TIMER) {
            continue;
        }

        uint32_t rld_mask = GPTMR_CH_RLD_STAT_MASK(i);
        uint32_t status = gptmr_get_status(base);
        if (status & rld_mask) {
            gptmr_clear_status(base, rld_mask);
            /* Ensure W1C clear reaches the peripheral before ISR return. */
            (void)gptmr_get_status(base);
            if (s_gptmr_state[ch].callback != NULL) {
                s_gptmr_state[ch].callback();
            }
        }
    }
}

SDK_DECLARE_EXT_ISR_M(IRQn_GPTMR0, isr_gptmr0)
void isr_gptmr0(void) { gptmr_isr_dispatch_instance(0); }

SDK_DECLARE_EXT_ISR_M(IRQn_GPTMR1, isr_gptmr1)
void isr_gptmr1(void) { gptmr_isr_dispatch_instance(1); }

SDK_DECLARE_EXT_ISR_M(IRQn_GPTMR2, isr_gptmr2)
void isr_gptmr2(void) { gptmr_isr_dispatch_instance(2); }

SDK_DECLARE_EXT_ISR_M(IRQn_GPTMR3, isr_gptmr3)
void isr_gptmr3(void) { gptmr_isr_dispatch_instance(3); }

/**
 * @brief 初始化 GPTMR 通道
 * @param ch 通道号
 * @param cfg 通道配置
 * @return 0 = 成功；-1 = 参数非法或 SDK 配置失败
 */
static int gptmr_drv_init(intf_gptmr_ch_t ch, const intf_gptmr_cfg_t *cfg)
{
    gptmr_channel_config_t hardware_cfg;

    if (cfg == NULL || !gptmr_ch_is_valid(ch)) {
        return -1;
    }
    if (cfg->frequency_hz == 0U) {
        return -1;
    }

    uint8_t inst = gptmr_inst_from_ch(ch);
    uint8_t lch = gptmr_local_ch(ch);
    GPTMR_Type *base = gptmr_get_base(ch);

    clock_add_to_group(s_gptmr_clocks[inst], 0);
    uint32_t clock_hz = clock_get_frequency(s_gptmr_clocks[inst]);
    if (clock_hz <= cfg->frequency_hz) {
        return -1;
    }

    gptmr_stop_counter(base, lch);
    gptmr_disable_irq(base, GPTMR_CH_RLD_IRQ_MASK(lch));
    gptmr_disable_irq(base, GPTMR_CH_CMP_IRQ_MASK(lch, 0));
    gptmr_disable_irq(base, GPTMR_CH_CMP_IRQ_MASK(lch, 1));
    gptmr_clear_status(base, GPTMR_CH_RLD_STAT_MASK(lch));
    gptmr_clear_status(base, GPTMR_CH_CMP_STAT_MASK(lch, 0));
    gptmr_clear_status(base, GPTMR_CH_CMP_STAT_MASK(lch, 1));

    gptmr_channel_get_default_config(base, &hardware_cfg);

    uint32_t reload = clock_hz / cfg->frequency_hz;
    hardware_cfg.reload = reload;
    hardware_cfg.enable_software_sync = cfg->enable_sync;
    hardware_cfg.enable_sync_follow_previous_channel = false;
    hardware_cfg.synci_edge = gptmr_synci_edge_none;

    /* 必须在 switch 之前：PWM/PWM_TIMER 分支的 gptmr_apply_duty() 依赖 reload
     * 计算 CMP0/CMP1（reload 为 0 时两者均为 0 → CMP0==CMP1 → 输出不翻转） */
    s_gptmr_state[ch].reload = reload;

    switch (cfg->mode) {
    case INTF_GPTMR_MODE_PWM:
        if (!gptmr_is_valid_duty(cfg->duty)) {
            return -1;
        }
        hardware_cfg.mode = gptmr_work_mode_no_capture;
        hardware_cfg.enable_cmp_output = true;
        hardware_cfg.cmp_initial_polarity_high = cfg->invert_output;
        if (gptmr_channel_config(base, lch, &hardware_cfg, false) != status_success) {
            return -1;
        }
        gptmr_channel_reset_count(base, lch);
        s_gptmr_state[ch].duty = cfg->duty;
        s_gptmr_state[ch].invert_output = cfg->invert_output;
        gptmr_apply_duty(ch, cfg->duty);
        break;

    case INTF_GPTMR_MODE_TIMER:
        if (cfg->callback == NULL) {
            return -1;
        }
        hardware_cfg.mode = gptmr_work_mode_no_capture;
        hardware_cfg.enable_cmp_output = false;
        if (gptmr_channel_config(base, lch, &hardware_cfg, false) != status_success) {
            return -1;
        }
        gptmr_channel_reset_count(base, lch);
        gptmr_enable_irq(base, GPTMR_CH_RLD_IRQ_MASK(lch));
        intc_m_enable_irq_with_priority(s_gptmr_irq_nums[inst], 3);
        break;

    case INTF_GPTMR_MODE_PWM_TIMER:
        if (!gptmr_is_valid_duty(cfg->duty) || cfg->callback == NULL) {
            return -1;
        }
        hardware_cfg.mode = gptmr_work_mode_no_capture;
        hardware_cfg.enable_cmp_output = true;
        hardware_cfg.cmp_initial_polarity_high = cfg->invert_output;
        if (gptmr_channel_config(base, lch, &hardware_cfg, false) != status_success) {
            return -1;
        }
        gptmr_channel_reset_count(base, lch);
        s_gptmr_state[ch].duty = cfg->duty;
        s_gptmr_state[ch].invert_output = cfg->invert_output;
        gptmr_apply_duty(ch, cfg->duty);
        gptmr_enable_irq(base, GPTMR_CH_RLD_IRQ_MASK(lch));
        intc_m_enable_irq_with_priority(s_gptmr_irq_nums[inst], 3);
        break;

    case INTF_GPTMR_MODE_CAPTURE:
        if (cfg->capture_edge > INTF_GPTMR_CAPTURE_EDGE_BOTH) {
            return -1;
        }
        hardware_cfg.mode = gptmr_get_capture_mode(cfg->capture_edge);
        hardware_cfg.enable_cmp_output = false;
        if (gptmr_channel_config(base, lch, &hardware_cfg, false) != status_success) {
            return -1;
        }
        gptmr_channel_reset_count(base, lch);
        break;

    default:
        return -1;
    }

    s_gptmr_state[ch].configured = true;
    s_gptmr_state[ch].mode = cfg->mode;
    s_gptmr_state[ch].frequency_hz = cfg->frequency_hz;
    s_gptmr_state[ch].callback = cfg->callback;
    return 0;
}

/**
 * @brief 启动 GPTMR 通道
 * @param ch 通道号
 * @return 0 = 成功；-1 = 通道未配置
 */
static int gptmr_drv_start(intf_gptmr_ch_t ch)
{
    if (!gptmr_ch_is_valid(ch) || !s_gptmr_state[ch].configured) {
        return -1;
    }

    uint8_t lch = gptmr_local_ch(ch);
    GPTMR_Type *base = gptmr_get_base(ch);

    gptmr_stop_counter(base, lch);
    gptmr_channel_reset_count(base, lch);
    gptmr_clear_status(base, GPTMR_CH_RLD_STAT_MASK(lch));

    if (s_gptmr_state[ch].mode == INTF_GPTMR_MODE_PWM
        || s_gptmr_state[ch].mode == INTF_GPTMR_MODE_PWM_TIMER) {
        gptmr_enable_cmp_output(base, lch);
    }

    gptmr_start_counter(base, lch);
    return 0;
}

/**
 * @brief 停止 GPTMR 通道
 * @param ch 通道号
 * @return 0 = 成功；-1 = 通道越界
 */
static int gptmr_drv_stop(intf_gptmr_ch_t ch)
{
    if (!gptmr_ch_is_valid(ch)) {
        return -1;
    }

    uint8_t lch = gptmr_local_ch(ch);
    GPTMR_Type *base = gptmr_get_base(ch);

    gptmr_disable_cmp_output(base, lch);
    gptmr_stop_counter(base, lch);
    return 0;
}

/**
 * @brief 设置 GPTMR 通道占空比
 * @param ch 通道号
 * @param duty 占空比
 * @return 0 = 成功；-1 = 通道未配置或非 PWM 模式
 */
static int gptmr_drv_set_duty(intf_gptmr_ch_t ch, float duty)
{
    if (!gptmr_ch_is_valid(ch) || !s_gptmr_state[ch].configured) {
        return -1;
    }
    if (s_gptmr_state[ch].mode != INTF_GPTMR_MODE_PWM
        && s_gptmr_state[ch].mode != INTF_GPTMR_MODE_PWM_TIMER) {
        return -1;
    }
    return gptmr_apply_duty(ch, duty);
}

/**
 * @brief 设置 GPTMR 通道频率（重载值）
 * @param ch 通道号
 * @param frequency_hz 频率 [Hz]
 * @return 0 = 成功；-1 = 参数非法或时钟不足
 */
static int gptmr_drv_set_frequency(intf_gptmr_ch_t ch, uint32_t frequency_hz)
{
    if (!gptmr_ch_is_valid(ch) || !s_gptmr_state[ch].configured) {
        return -1;
    }
    if (frequency_hz == 0U) {
        return -1;
    }

    uint8_t inst = gptmr_inst_from_ch(ch);
    uint8_t lch = gptmr_local_ch(ch);
    GPTMR_Type *base = gptmr_get_base(ch);

    clock_add_to_group(s_gptmr_clocks[inst], 0);
    uint32_t clock_hz = clock_get_frequency(s_gptmr_clocks[inst]);
    if (clock_hz <= frequency_hz) {
        return -1;
    }

    s_gptmr_state[ch].reload = clock_hz / frequency_hz;
    s_gptmr_state[ch].frequency_hz = frequency_hz;

    gptmr_stop_counter(base, lch);
    gptmr_channel_config_update_reload(base, lch, s_gptmr_state[ch].reload);
    gptmr_channel_reset_count(base, lch);

    if (s_gptmr_state[ch].mode == INTF_GPTMR_MODE_PWM
        || s_gptmr_state[ch].mode == INTF_GPTMR_MODE_PWM_TIMER) {
        gptmr_apply_duty(ch, s_gptmr_state[ch].duty);
    }

    gptmr_start_counter(base, lch);
    return 0;
}

/**
 * @brief 强制通道输出低电平
 * @param ch 通道号
 * @return 0 = 成功；-1 = 通道未配置
 */
static int gptmr_drv_force_low(intf_gptmr_ch_t ch)
{
    if (!gptmr_ch_is_valid(ch) || !s_gptmr_state[ch].configured) {
        return -1;
    }

    uint8_t lch = gptmr_local_ch(ch);
    GPTMR_Type *base = gptmr_get_base(ch);

    gptmr_stop_counter(base, lch);
    gptmr_update_cmp(base, lch, 0, 0xFFFFFFFFU);
    gptmr_update_cmp(base, lch, 1, 0xFFFFFFFFU);
    gptmr_enable_cmp_output(base, lch);
    gptmr_channel_reset_count(base, lch);
    gptmr_start_counter(base, lch);
    return 0;
}

/**
 * @brief 解除强制低电平，恢复 PWM 输出
 * @param ch 通道号
 * @return 0 = 成功；-1 = 通道未配置
 */
static int gptmr_drv_force_release(intf_gptmr_ch_t ch)
{
    if (!gptmr_ch_is_valid(ch) || !s_gptmr_state[ch].configured) {
        return -1;
    }

    uint8_t lch = gptmr_local_ch(ch);
    GPTMR_Type *base = gptmr_get_base(ch);

    gptmr_stop_counter(base, lch);
    gptmr_update_cmp(base, lch, 0,
        (uint32_t)((float)s_gptmr_state[ch].reload * s_gptmr_state[ch].duty));
    gptmr_update_cmp(base, lch, 1, s_gptmr_state[ch].reload);
    gptmr_enable_cmp_output(base, lch);
    gptmr_channel_reset_count(base, lch);
    gptmr_start_counter(base, lch);
    return 0;
}

/**
 * @brief 轮询输入捕获（首边沿起始，次边沿出周期）
 * @param ch 通道号
 * @param capture 捕获结果输出
 * @return 0 = 成功；-1 = 通道非法或非捕获模式
 */
static int gptmr_drv_capture_poll(intf_gptmr_ch_t ch, intf_gptmr_capture_t *capture)
{
    static bool has_first_edge[GPTMR_TOTAL_CHANNELS] = {false};
    static uint32_t first_count[GPTMR_TOTAL_CHANNELS] = {0};
    static uint32_t period_ticks[GPTMR_TOTAL_CHANNELS] = {0};

    if ((capture == NULL) || !gptmr_ch_is_valid(ch)
        || !s_gptmr_state[ch].configured
        || s_gptmr_state[ch].mode != INTF_GPTMR_MODE_CAPTURE) {
        return -1;
    }

    uint8_t lch = gptmr_local_ch(ch);
    GPTMR_Type *base = gptmr_get_base(ch);

    capture->captured = false;
    capture->count = 0;
    capture->period_ticks = period_ticks[ch];

    if (!gptmr_check_status(base, GPTMR_CH_CAP_STAT_MASK(lch))) {
        return 0;
    }

    gptmr_clear_status(base, GPTMR_CH_CAP_STAT_MASK(lch));
    uint32_t count = gptmr_channel_get_counter(base, lch,
        gptmr_get_capture_counter_type(INTF_GPTMR_CAPTURE_EDGE_RISING));
    capture->count = count;

    if (!has_first_edge[ch]) {
        first_count[ch] = count;
        has_first_edge[ch] = true;
        return 0;
    }

    period_ticks[ch] = gptmr_calc_delta(first_count[ch], count);
    first_count[ch] = count;
    capture->captured = true;
    capture->period_ticks = period_ticks[ch];
    return 0;
}

#define GPTMR_OPS_INITIALIZER(inst_id) { \
    .instance_id = (inst_id),            \
    .init = gptmr_drv_init,              \
    .start = gptmr_drv_start,            \
    .stop = gptmr_drv_stop,              \
    .set_duty = gptmr_drv_set_duty,      \
    .set_frequency = gptmr_drv_set_frequency, \
    .force_low = gptmr_drv_force_low,    \
    .force_release = gptmr_drv_force_release, \
    .capture_poll = gptmr_drv_capture_poll, \
}

static const intf_gptmr_t s_gptmr_ops[GPTMR_INSTANCE_COUNT] = {
    GPTMR_OPS_INITIALIZER(0),
    GPTMR_OPS_INITIALIZER(1),
    GPTMR_OPS_INITIALIZER(2),
    GPTMR_OPS_INITIALIZER(3),
};

void hpm_gptmr_driver_register(void)
{
    for (uint8_t i = 0; i < GPTMR_INSTANCE_COUNT; i++) {
        intf_gptmr_register(&s_gptmr_ops[i]);
    }
}
