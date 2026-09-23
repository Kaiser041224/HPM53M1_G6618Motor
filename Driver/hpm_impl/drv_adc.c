/**
 * @file    drv_adc.c
 * @brief   ADC 驱动 - HPM ADC16 硬件实现（PMT/SEQ/Period/Oneshot）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_adc.h"

#include "hpm_adc16_drv.h"
#include "hpm_clock_drv.h"
#include "hpm_common.h"
#include "hpm_interrupt.h"
#include "hpm_misc.h"
#include "hpm_soc_irq.h"
#include "hpm_sysctl_drv.h"

#include "irq_profiler.h"

#include <stddef.h>
#include <string.h>

#define ADC_DEFAULT_VREF_MV      INTF_ADC_DEFAULT_VREF_MV
#define ADC_DEFAULT_SAMPLE_CYCLE INTF_ADC_DEFAULT_SAMPLE_CYCLE
#define ADC_DEFAULT_CLOCK_DIV    INTF_ADC_DEFAULT_CLOCK_DIV
#define ADC_MAX_CLOCK_DIV        (16U)
#define ADC_MAX_CLOCK_HZ         (50000000U)
#define ADC_CONV_CYCLES          (25U)
#define ADC_MAX_CHANNELS         (16U)
#define ADC_PMT_MAX_TRIG         (11U)
#define ADC_PMT_DMA_SLOT_LEN     (4U)
/* PMT DMA 字 bit31：cycle bit（SDK 参考协议 —— 硬件写新数据置 1，软件消费后清 0） */
#define ADC_PMT_CYCLE_BIT_MASK   (0x80000000UL)
#define ADC_SEQ_MAX_LEN          16U

/* Discard first N PMT trigger completions to avoid startup transient garbage */
#define ADC_PMT_STARTUP_DISCARD (8U)

/* PMT 结果缓冲：12 触发 × 4 槽 × 1 字 = 48 字，须位于非缓存本地 RAM（DMA 直接写入） */
#define ADC_PMT_DMA_WORDS (48U)
ATTR_PLACE_AT_FAST_RAM_BSS static uint32_t s_adc_pmt_dma_buff[INTF_ADC_INSTANCE_COUNT][ADC_PMT_DMA_WORDS];

/* ============================================================================
 * Instance State
 * ============================================================================ */

/**
 * @brief ADC 通道运行状态
 */
typedef struct {
    bool configured; /**< 是否已配置 */
    bool running;    /**< 是否已启动 */
} adc_ch_state_t;

/**
 * @brief ADC 实例状态
 */
typedef struct {
    bool initialized;                          /**< 是否已初始化 */
    intf_adc_resolution_t resolution;          /**< 分辨率 */
    intf_adc_mode_t mode;                      /**< 转换模式 */
    float vref_mv;                             /**< 参考电压 [mV] */
    uint32_t period_rate_hz;                   /**< Period 模式目标速率 [Hz] */
    ADC16_Type* base;                          /**< ADC 寄存器基地址 */
    uint32_t irq;                              /**< 中断号 */
    adc_ch_state_t channels[ADC_MAX_CHANNELS]; /**< 各通道状态 */
    /* PMT */
    struct {
        uint8_t trig_ch;                       /**< 触发通道 */
        uint8_t ch_count;                      /**< 队列通道数 */
        uint8_t ch_list[4];                    /**< 队列通道列表 */
        intf_adc_pmt_cb_t cb;                  /**< PMT 回调 */
        void* cb_user_data;                    /**< PMT 回调用户数据 */
        uint32_t frame_cnt;                    /**< 帧计数 */
        /* cycle-bit 消费协议（SDK 参考设计）：硬件写新数据置 1，软件读后清 0。
         * 连续 3 帧全槽无 cycle bit 时判定硬件不复位该位 → 自动关闭协议并回退
         * 到"不检查 cycle bit"的兼容模式（不会把数据全部拒死）。 */
        bool cycle_protocol;                   /**< 是否启用 cycle-bit 校验协议 */
        uint8_t stale_run;                     /**< 连续无 cycle bit 帧数 */
    } pmt;
    /* Sequence */
    struct {
        bool hw_trig;                          /**< 是否硬件触发 */
        uint8_t ch_count;                      /**< 序列通道数 */
        uint8_t ch_list[ADC_SEQ_MAX_LEN];      /**< 序列通道列表 */
        intf_adc_seq_cb_t cb;                  /**< 序列回调 */
        void* cb_user_data;                    /**< 序列回调用户数据 */
    } seq;
    /* DMA (shared by PMT and Seq) */
    struct {
        bool active;                           /**< 是否启用 DMA */
        uint32_t* buff;                        /**< DMA 缓冲 */
        uint32_t len;                          /**< DMA 缓冲长度 [字] */
    } dma;
    /* Watchdog */
    struct {
        bool enabled[ADC_MAX_CHANNELS];        /**< 各通道 WDOG 使能 */
        intf_adc_wdog_cb_t cb;                 /**< WDOG 回调 */
        void* cb_user_data;                    /**< WDOG 回调用户数据 */
    } wdog;
} adc_inst_t;

ATTR_PLACE_AT_FAST_RAM_BSS static adc_inst_t s_adc_instances[INTF_ADC_INSTANCE_COUNT];
ATTR_PLACE_AT_FAST_RAM_BSS static volatile intf_adc_diag_snapshot_t s_adc_diag;

/* 累计 ISR 总 cycle 数：由诊断接口输出，主循环按墙钟周期求差得到真实 CPU 占用率
 * （取代"最坏值×频率"外推法）。低 32 位即可满足增量统计。 */
volatile uint64_t g_adc_isr_total_cycles[INTF_ADC_INSTANCE_COUNT];

/* ============================================================================
 * Hardware Mapping Helpers
 * ============================================================================ */

/**
 * @brief 获取 ADC 寄存器基地址
 * @param inst 实例号
 * @return ADC 基地址；越界返回 NULL
 */
static ADC16_Type* adc_get_base(uint8_t inst) {
    switch (inst) {
    case 0: return HPM_ADC0;
    case 1: return HPM_ADC1;
    default: return NULL;
    }
}

/**
 * @brief 获取 ADC 外设时钟名
 * @param inst 实例号
 * @return 时钟名；越界返回 0
 */
static clock_name_t adc_get_clock(uint8_t inst) {
    switch (inst) {
    case 0: return clock_adc0;
    case 1: return clock_adc1;
    default: return (clock_name_t)0;
    }
}

/**
 * @brief 获取 ADC 中断号
 * @param inst 实例号
 * @return 中断号；越界返回 0
 */
static uint32_t adc_get_irq(uint8_t inst) {
    switch (inst) {
    case 0: return IRQn_ADC0;
    case 1: return IRQn_ADC1;
    default: return 0;
    }
}

/**
 * @brief 初始化 ADC 时钟源（AHB0）
 * @param inst 实例号
 * @return 0 = 成功；-1 = 实例越界或时钟失败
 */
static int adc_init_clock(uint8_t inst) {
    if (inst >= INTF_ADC_INSTANCE_COUNT)
        return -1;

    clock_name_t clock = adc_get_clock(inst);

    clock_add_to_group(clock, 0);
    if (clock_set_adc_source(clock, clk_adc_src_ahb0) != status_success)
        return -1;

    return (clock_get_frequency(clock) > 0) ? 0 : -1;
}

/**
 * @brief 分辨率枚举映射为 SDK 值
 * @param res 分辨率枚举
 * @return SDK 分辨率
 */
static adc16_resolution_t adc_map_resolution(intf_adc_resolution_t res) {
    switch (res) {
    case INTF_ADC_RES_8_BITS: return adc16_res_8_bits;
    case INTF_ADC_RES_10_BITS: return adc16_res_10_bits;
    case INTF_ADC_RES_12_BITS: return adc16_res_12_bits;
    case INTF_ADC_RES_16_BITS:
    default: return adc16_res_16_bits;
    }
}

/**
 * @brief 分辨率对应的最大原始码
 * @param res 分辨率枚举
 * @return 最大原始码
 */
static uint16_t adc_resolution_max_value(intf_adc_resolution_t res) {
    switch (res) {
    case INTF_ADC_RES_8_BITS: return (uint16_t)0xFF;
    case INTF_ADC_RES_10_BITS: return (uint16_t)0x3FF;
    case INTF_ADC_RES_12_BITS: return (uint16_t)0xFFF;
    case INTF_ADC_RES_16_BITS:
    default: return (uint16_t)0xFFFF;
    }
}

/**
 * @brief 转换模式枚举映射为 SDK 值
 * @param mode 模式枚举
 * @return SDK 转换模式
 */
static adc16_conversion_mode_t adc_map_mode(intf_adc_mode_t mode) {
    switch (mode) {
    case INTF_ADC_MODE_PERIOD: return adc16_conv_mode_period;
    case INTF_ADC_MODE_PMT: return adc16_conv_mode_preemption;
    case INTF_ADC_MODE_SEQ: return adc16_conv_mode_sequence;
    case INTF_ADC_MODE_ONESHOT:
    default: return adc16_conv_mode_oneshot;
    }
}

/**
 * @brief 计算 ADC 时钟分频（强制 ADC 时钟 ≤ 50MHz）
 * @param inst 实例号
 * @param sample_rate_hz 目标采样率 [Hz]（0 = 用默认分频）
 * @param user_div 用户指定分频（0 = 自动）
 * @return 分频值
 */
static uint32_t adc_calc_clock_div(uint8_t inst, uint32_t sample_rate_hz, uint32_t user_div) {
    if (user_div >= 1 && user_div <= ADC_MAX_CLOCK_DIV) {
        uint32_t bus_freq = clock_get_frequency(adc_get_clock(inst));
        uint32_t adc_clk = bus_freq / user_div;
        if (adc_clk > ADC_MAX_CLOCK_HZ) {
            /* enforce datasheet limit: ADC clock ≤ 50 MHz */
            return (bus_freq + ADC_MAX_CLOCK_HZ - 1) / ADC_MAX_CLOCK_HZ;
        }
        return user_div;
    }

    if (sample_rate_hz == 0) {
        uint32_t bus_freq = clock_get_frequency(adc_get_clock(inst));
        uint32_t min_safe_div = (bus_freq + ADC_MAX_CLOCK_HZ - 1) / ADC_MAX_CLOCK_HZ;

        return (ADC_DEFAULT_CLOCK_DIV < min_safe_div) ? min_safe_div : ADC_DEFAULT_CLOCK_DIV;
    }

    uint32_t bus_freq = clock_get_frequency(adc_get_clock(inst));
    uint32_t needed_clk = sample_rate_hz * ADC_CONV_CYCLES;
    uint32_t div = bus_freq / needed_clk;

    if (div < 1)
        div = 1;
    if (div > ADC_MAX_CLOCK_DIV)
        div = ADC_MAX_CLOCK_DIV;

    /* enforce datasheet limit: ADC clock ≤ 50 MHz */
    uint32_t min_safe_div = (bus_freq + ADC_MAX_CLOCK_HZ - 1) / ADC_MAX_CLOCK_HZ;
    if (div < min_safe_div)
        div = min_safe_div;

    return div;
}

/**
 * @brief 使能实例中断并设置 PLIC 优先级
 * @param inst 实例号
 */
static void adc_enable_instance_irq(uint8_t inst) {
    adc_inst_t* ai = &s_adc_instances[inst];
    uint32_t irq = ai->irq;
    if (irq != 0) {
        /* PLIC: 数字越大优先级越高。ADC0 负责 25kHz FOC 控制环路（PMT 完成中断），
         * 优先级最高（3），高于 ADC1 的慢通道/故障 tick（1）与其它外设 ISR。
         * 对齐 freertos-foc-fastlane 设计 §6 优先级契约：
         * 硬件故障关断 > FOC 快车道(ADC0 PMT) > 其它外设 ISR > RTOS 任务。 */
        uint32_t priority = (inst == 0U) ? 3U : 1U;
        intc_m_enable_irq_with_priority(irq, priority);
    }
}

/* ============================================================================
 * ISR
 * ============================================================================ */

/**
 * @brief ADC 通用中断服务（PMT 完成 / SEQ 完成 / WDOG 越限）
 * @param inst 实例号
 */
ATTR_RAMFUNC
static void adc_generic_isr(uint8_t inst) {
    uint32_t t0 = irq_prof_read_cycle();

    s_adc_diag.generic_entry[inst]++;

    adc_inst_t* ai = &s_adc_instances[inst];
    if (!ai->initialized)
        return;

    ADC16_Type* base = ai->base;
    uint32_t status = adc16_get_status_flags(base);
    adc16_clear_status_flags(base, status);

    /* PMT trigger complete */
    if (ADC16_INT_STS_TRIG_CMPT_GET(status) && ai->mode == INTF_ADC_MODE_PMT) {
        s_adc_diag.pmt_complete[inst]++;
        ai->pmt.frame_cnt++;
        if (ai->pmt.frame_cnt < ADC_PMT_STARTUP_DISCARD) {
            s_adc_diag.pmt_startup_drop[inst]++;
            return;
        }

        /* 首次有效帧后使能 WDOG 中断（SoC 延迟式：避免启动期误报；
         * adc16_enable_wdog_interrupt 先清挂起标志再置 INT_EN） */
        if (ai->pmt.frame_cnt == ADC_PMT_STARTUP_DISCARD) {
            uint32_t wdog_mask = 0U;

            for (uint8_t ch = 0U; ch < ADC_MAX_CHANNELS; ch++) {
                if (ai->wdog.enabled[ch]) {
                    wdog_mask |= (uint32_t) (1u << ch);
                }
            }
            if (wdog_mask != 0U) {
                adc16_enable_wdog_interrupt(base, wdog_mask);
            }
        }

        if (ai->pmt.cb && ai->pmt.ch_count > 0) {
            uint16_t values[4];
            uint8_t valid = 0;

            if (ai->dma.active) {
                uint32_t dma_offset = (uint32_t)ai->pmt.trig_ch * ADC_PMT_DMA_SLOT_LEN;
                volatile uint32_t* dma_hw = &ai->dma.buff[dma_offset];

                uint32_t mstatus = disable_global_irq(CSR_MSTATUS_MIE_MASK);

                uint32_t snap[4];
                snap[0] = dma_hw[0];
                snap[1] = dma_hw[1];
                snap[2] = dma_hw[2];
                snap[3] = dma_hw[3];

                /* cycle bit 统计（硬件写入新数据时置 1）+ 原始帧转储 */
                uint8_t fresh = 0U;
                for (uint8_t i = 0; i < 4; i++) {
                    if ((snap[i] & ADC_PMT_CYCLE_BIT_MASK) != 0U) {
                        fresh++;
                    }
                    s_adc_diag.pmt_last[inst][i] = snap[i];
                }

                /* 协议自检：连续多帧全槽无 cycle bit → 硬件不复位该位，回退兼容模式 */
                if (ai->pmt.cycle_protocol) {
                    if (fresh == 0U) {
                        if (++ai->pmt.stale_run >= 3U) {
                            ai->pmt.cycle_protocol = false;
                            s_adc_diag.pmt_cycle_fallback[inst]++;
                        }
                    } else {
                        ai->pmt.stale_run = 0U;
                    }

                    /* 消费（清除）cycle bit：硬件写新数据置 1，软件读后清 0；
                     * 下一帧若某槽仍为 0 → 该槽未被硬件更新（数据过期），校验拒绝。 */
                    for (uint8_t i = 0; i < 4; i++) {
                        if ((snap[i] & ADC_PMT_CYCLE_BIT_MASK) != 0U) {
                            dma_hw[i] = snap[i] & ~ADC_PMT_CYCLE_BIT_MASK;
                        }
                    }
                }

                restore_global_irq(mstatus);

                /* 从 DMA 结果字直接位运算提取字段，避免 (adc16_pmt_dma_data_t*)snap
                 * 类型双关：-O2/-O3 的 strict-aliasing + DSE 会消除 snap[] 赋值，
                 * 导致读到未初始化栈值、PMT 校验全失败、控制回调不执行。位布局与
                 * hpm_adc16_drv.h 的 adc16_pmt_dma_data_t 一致(此 SoC IP_VERSION>=2)。 */
#if defined(ADC_SOC_IP_VERSION) && (ADC_SOC_IP_VERSION < 2)
#define ADC_PMT_RESULT(w)    ((uint16_t)((w) & 0xFFFFU))
#define ADC_PMT_TRIG_CH(w)   ((uint8_t)(((w) >> 22) & 0x0FU))
#define ADC_PMT_ADC_CH(w)    ((uint8_t)(((w) >> 26) & 0x1FU))
#define ADC_PMT_CYCLE_BIT(w) ((uint8_t)(((w) >> 31) & 0x01U))
#else
#define ADC_PMT_RESULT(w)    ((uint16_t)((w) & 0xFFFFU))
#define ADC_PMT_ADC_CH(w)    ((uint8_t)(((w) >> 20) & 0x1FU))
#define ADC_PMT_TRIG_CH(w)   ((uint8_t)(((w) >> 25) & 0x0FU))
#define ADC_PMT_CYCLE_BIT(w) ((uint8_t)(((w) >> 31) & 0x01U))
#endif
/* 队列内转换序号（PMT 字 bit[30:29]） */
#define ADC_PMT_SEQ_NUM(w)   ((uint8_t)(((w) >> 29) & 0x03U))
                for (uint8_t i = 0; i < ai->pmt.ch_count && i < 4; i++) {
                    uint32_t raw_word = snap[i];
                    if (ai->pmt.cycle_protocol && (ADC_PMT_CYCLE_BIT(raw_word) == 0)) {
                        s_adc_diag.pmt_invalid_cycle[inst]++;
                        continue;
                    }
                    if (ADC_PMT_TRIG_CH(raw_word) != ai->pmt.trig_ch) {
                        s_adc_diag.pmt_invalid_trig[inst]++;
                        continue;
                    }
                    if (ADC_PMT_ADC_CH(raw_word) != ai->pmt.ch_list[i]) {
                        s_adc_diag.pmt_invalid_channel[inst]++;
                        continue;
                    }
                    values[valid] = ADC_PMT_RESULT(raw_word);
                    valid++;
                }
            } else {
                for (uint8_t i = 0; i < ai->pmt.ch_count && i < 4; i++) {
                    uint32_t bus_res = base->BUS_RESULT[ai->pmt.ch_list[i]];
                    if (ADC16_BUS_RESULT_VALID_GET(bus_res)) {
                        values[valid] = ADC16_BUS_RESULT_CHAN_RESULT_GET(bus_res);
                        valid++;
                    }
                }
            }

            if (valid == ai->pmt.ch_count) {
                s_adc_diag.pmt_callback[inst]++;
                ai->pmt.cb(INTF_ADC_CH(inst, ai->pmt.trig_ch), values, valid, ai->pmt.cb_user_data);
            } else {
                s_adc_diag.pmt_invalid[inst]++;
            }
        }
    }

    /* Sequence full queue complete（每帧一次；CVC 为末通道单次完成，不单独回调） */
    if (ADC16_INT_STS_SEQ_CMPT_GET(status) && ai->mode == INTF_ADC_MODE_SEQ) {
        if (ai->seq.cb) {
            ai->seq.cb(INTF_ADC_CH(inst, 0), ai->seq.cb_user_data);
        }
    }

    /* Watchdog threshold violation */
    uint32_t wdog_status = ADC16_INT_STS_WDOG_GET(status);
    if (wdog_status) {
        for (uint8_t ch = 0; ch < ADC_MAX_CHANNELS; ch++) {
            if ((wdog_status & (1u << ch)) && ai->wdog.enabled[ch]) {
                /* PRD_RESULTx 保存通道 x 最近一次转换结果（所有模式通用，PMT 已实测） */
                uint16_t val = (uint16_t) ADC16_PRD_CFG_PRD_RESULT_CHAN_RESULT_GET(
                    base->PRD_CFG[ch].PRD_RESULT);
                if (ai->wdog.cb) {
                    ai->wdog.cb(INTF_ADC_CH(inst, ch), val, ai->wdog.cb_user_data);
                }
                adc16_disable_interrupts(base, (uint32_t)(1u << ch));
            }
        }
    }

    uint32_t elapsed = irq_prof_read_cycle() - t0;
    if (elapsed > s_adc_diag.isr_cycles_max[inst]) {
        s_adc_diag.isr_cycles_max[inst] = elapsed;
    }
    g_adc_isr_total_cycles[inst] += elapsed; /* 累计总占用 */
}

SDK_DECLARE_EXT_ISR_M(IRQn_ADC0, isr_adc0)
void isr_adc0(void) {
    irq_prof_nest_enter(); /* [TEMP DIAG] */
    s_adc_diag.irq_entry[0]++;
    adc_generic_isr(0);
    irq_prof_nest_exit(); /* [TEMP DIAG] */
}

SDK_DECLARE_EXT_ISR_M(IRQn_ADC1, isr_adc1)
void isr_adc1(void) {
    irq_prof_nest_enter(); /* [TEMP DIAG] */
    s_adc_diag.irq_entry[1]++;
    adc_generic_isr(1);
    irq_prof_nest_exit(); /* [TEMP DIAG] */
}

int adc_get_diag_snapshot(intf_adc_diag_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return -1;
    }

    uint32_t mstatus = disable_global_irq(CSR_MSTATUS_MIE_MASK);
    *snapshot = s_adc_diag;
    restore_global_irq(mstatus);

    for (uint8_t inst = 0; inst < INTF_ADC_INSTANCE_COUNT; inst++) {
        ADC16_Type* base = s_adc_instances[inst].base;

        snapshot->isr_total_cycles[inst] = g_adc_isr_total_cycles[inst];
        if (base != NULL) {
            snapshot->reg_conv_cfg1[inst] = base->CONV_CFG1;
            snapshot->reg_adc_cfg0[inst] = base->ADC_CFG0;
            snapshot->reg_buf_cfg0[inst] = base->BUF_CFG0;
            snapshot->reg_config0[inst] = base->CONFIG[ADC16_CONFIG_TRG0A];
            snapshot->reg_int_sts[inst] = base->INT_STS;
            snapshot->reg_seq_cfg0[inst] = base->SEQ_CFG0;
            for (uint8_t ch = 0U; ch < 16U; ch++) {
                snapshot->reg_prd_result[inst][ch] = base->PRD_CFG[ch].PRD_RESULT;
            }
        }
    }

    return 0;
}

void adc_reset_diag_max(void)
{
    for (uint8_t i = 0; i < INTF_ADC_INSTANCE_COUNT; i++) {
        s_adc_diag.isr_cycles_max[i] = 0;
    }
}

/* ============================================================================
 * WDOG re-enable helper (public for App manual re-arm)
 * ============================================================================ */

void adc_wdog_reenable(uint8_t inst, uint8_t ch) {
    if (inst >= INTF_ADC_INSTANCE_COUNT || ch >= ADC_MAX_CHANNELS)
        return;
    adc_inst_t* ai = &s_adc_instances[inst];
    if (!ai->initialized || !ai->wdog.enabled[ch])
        return;
    /* 先清挂起标志再使能（避免陈旧标志立即重触发） */
    adc16_enable_wdog_interrupt(ai->base, (uint32_t)(1u << ch));
}

/* ============================================================================
 * HPM ADC16 Implementation
 * ============================================================================ */

/**
 * @brief 初始化 ADC 通道（首次调用含实例全局初始化）
 * @param ch 逻辑通道
 * @param cfg 通道配置
 * @return 0 = 成功；-1 = 参数非法或 SDK 初始化失败
 */
static int adc_init(intf_adc_ch_t ch, const intf_adc_cfg_t* cfg) {
    uint8_t inst = INTF_ADC_CH_INST(ch);
    uint8_t ch_idx = INTF_ADC_CH_IDX(ch);

    if (inst >= INTF_ADC_INSTANCE_COUNT || cfg == NULL)
        return -1;
    if (cfg->mode != INTF_ADC_MODE_PMT && cfg->mode != INTF_ADC_MODE_SEQ
        && ch_idx >= ADC_MAX_CHANNELS)
        return -1;

    adc_inst_t* ai = &s_adc_instances[inst];
    uint32_t sample_cycle = (cfg->sample_cycle > 0) ? cfg->sample_cycle : ADC_DEFAULT_SAMPLE_CYCLE;

    /* --- global instance init (first call) --- */
    if (!ai->initialized) {
        ADC16_Type* base = adc_get_base(inst);
        if (base == NULL)
            return -1;
        if (adc_init_clock(inst) != 0)
            return -1;

        adc16_config_t adc_cfg;
        adc16_get_default_config(&adc_cfg);

        adc_cfg.res = adc_map_resolution(cfg->resolution);
        adc_cfg.conv_mode = adc_map_mode(cfg->mode);
        adc_cfg.adc_clk_div =
            (adc16_clock_divider_t)adc_calc_clock_div(inst, cfg->sample_rate_hz, cfg->clock_div);
        /* WAIT_DIS 语义（手册 §53.2.3）：
         *   0 = 阻塞读：读 BUS_RESULT 触发转换，待完成后返回【本次】结果；
         *   1 = 非阻塞：立即返回【上一次】转换结果（VALID 位提示完成）。
         * 读取模式（oneshot）需要每次读取都是新鲜值 → 用阻塞读；
         * PMT/SEQ 等模式不做 BUS_RESULT 读取 → 保持非阻塞。 */
        adc_cfg.wait_dis = (cfg->mode != INTF_ADC_MODE_ONESHOT);

        /* SEL_SYNC_AHB=1 要求 ADC 时钟 == 总线时钟（手册 §53.4 ADC_CFG0）；
         * 本板 ADC 时钟为 AHB/4，故所有模式统一 sel_sync_ahb=false。
         * adc_ahb_en 仅对序列/抢占的 DMA 有效，oneshot 关闭。 */
        if (adc_cfg.conv_mode == adc16_conv_mode_oneshot) {
            adc_cfg.sel_sync_ahb = false;
            adc_cfg.adc_ahb_en = false;
        } else {
            adc_cfg.sel_sync_ahb = false;
            adc_cfg.adc_ahb_en = true;
        }

        if (adc16_init(base, &adc_cfg) != status_success)
            return -1;

        base->ANA_CTRL0 |= ADC16_ANA_CTRL0_ADC_CLK_ON_MASK;

        /* 关闭全部抢占队列：防止复位残留的 QUEUE_EN 位造成误触发 */
        for (uint8_t t = 0; t < ADC_PMT_MAX_TRIG; t++) {
            adc16_disable_pmt_queue(base, t);
        }

        ai->initialized = true;
        ai->resolution = cfg->resolution;
        ai->mode = cfg->mode;
        ai->base = base;
        ai->irq = adc_get_irq(inst);
        ai->vref_mv = (cfg->vref_mv > 0.0f) ? cfg->vref_mv : ADC_DEFAULT_VREF_MV;
        ai->period_rate_hz = (cfg->sample_rate_hz != 0U) ? cfg->sample_rate_hz : 1000U;
    } else {
        if (ai->resolution != cfg->resolution || ai->mode != cfg->mode)
            return -1;
    }

    /* DMA hardware support: only PMT and Sequence modes have DMA engines.
     * Oneshot/Period modes must read results via CPU (BUS_RESULT/PRD_RESULT). */
    if (cfg->dma_en && ai->mode != INTF_ADC_MODE_PMT && ai->mode != INTF_ADC_MODE_SEQ) {
        return -1;
    }

    /* --- per-mode init --- */

    if (ai->mode == INTF_ADC_MODE_PMT) {
        if (cfg->pmt_ch_count == 0 || cfg->pmt_ch_count > 4 || cfg->pmt_trig_ch >= ADC_PMT_MAX_TRIG)
            return -1;
        for (uint8_t i = 0; i < cfg->pmt_ch_count; i++) {
            if (cfg->pmt_ch_list[i] >= ADC_MAX_CHANNELS)
                return -1;
        }

        ai->pmt.trig_ch = cfg->pmt_trig_ch;
        ai->pmt.ch_count = cfg->pmt_ch_count;
        ai->pmt.cb = cfg->pmt_cb;
        ai->pmt.cb_user_data = cfg->pmt_cb_user_data;

        for (uint8_t i = 0; i < cfg->pmt_ch_count; i++) {
            ai->pmt.ch_list[i] = cfg->pmt_ch_list[i];
        }

        adc16_channel_config_t ch_cfg;
        adc16_get_channel_default_config(&ch_cfg);
        ch_cfg.sample_cycle = sample_cycle;
        if (cfg->wdog_en) {
            ch_cfg.wdog_int_en = true;
            ch_cfg.thshdh = cfg->wdog_thshd_high;
            ch_cfg.thshdl = cfg->wdog_thshd_low;
        }

        for (uint8_t i = 0; i < cfg->pmt_ch_count; i++) {
            ch_cfg.ch = cfg->pmt_ch_list[i];
            if (adc16_init_channel(ai->base, &ch_cfg) != status_success)
                return -1;
            ai->channels[cfg->pmt_ch_list[i]].configured = true;
            if (cfg->wdog_en) {
                ai->wdog.enabled[cfg->pmt_ch_list[i]] = true;
                ai->wdog.cb = cfg->wdog_cb;
                ai->wdog.cb_user_data = cfg->wdog_cb_user_data;
                /* INT_EN 延迟到首次有效帧后再置（SoC 延迟式，见 ISR 内 arm） */
            }
        }

        adc16_pmt_config_t pmt_cfg;
        pmt_cfg.trig_ch = cfg->pmt_trig_ch;
        pmt_cfg.trig_len = cfg->pmt_ch_count;
        for (uint8_t i = 0; i < cfg->pmt_ch_count; i++) {
            pmt_cfg.adc_ch[i] = cfg->pmt_ch_list[i];
            pmt_cfg.inten[i] = ((i + 1) == cfg->pmt_ch_count);
        }

        if (adc16_set_pmt_config(ai->base, &pmt_cfg) != status_success)
            return -1;
        adc16_enable_pmt_queue(ai->base, cfg->pmt_trig_ch);

        /* Explicitly disable all other trig_ch — hardware reset may leave
         * stale QUEUE_EN bits.  Without this, cross-triggering from shared
         * PTRGI inputs causes the PMT state machine to process garbage
         * channel lists, corrupting subsequent conversion results. */
        for (uint8_t t = 0; t < ADC_PMT_MAX_TRIG; t++) {
            if (t != cfg->pmt_trig_ch) {
                adc16_disable_pmt_queue(ai->base, t);
            }
        }

        /* 无回调（如慢速通道仅轮询 DMA 缓冲）时不使能中断，避免无谓的 25kHz ISR */
        if (cfg->pmt_cb != NULL) {
            adc16_enable_interrupts(ai->base, adc16_event_trig_complete);
        }

        if (cfg->dma_en) {
            /* dma_buff = NULL：使用驱动内部 fast RAM 缓冲（推荐） */
            uint32_t* dma_buff = (cfg->dma_buff != NULL) ? cfg->dma_buff : s_adc_pmt_dma_buff[inst];
            uint32_t dma_len = (cfg->dma_buff != NULL) ? cfg->dma_buff_len : ADC_PMT_DMA_WORDS;
            uint32_t dma_offset = (uint32_t)cfg->pmt_trig_ch * ADC_PMT_DMA_SLOT_LEN;

            if (dma_len < dma_offset + cfg->pmt_ch_count)
                return -1;

            ai->dma.active = true;
            ai->dma.buff = dma_buff;
            ai->dma.len = dma_len;
            adc16_init_pmt_dma(ai->base, core_local_mem_to_sys_address(0, (uint32_t)dma_buff));
        }

#if defined(HPM_IP_FEATURE_ADC16_HAS_MOT_EN) && HPM_IP_FEATURE_ADC16_HAS_MOT_EN
        adc16_enable_motor(ai->base);
#endif
        ai->pmt.frame_cnt = 0;
        ai->pmt.cycle_protocol = true;
        ai->pmt.stale_run = 0U;

        if ((cfg->pmt_cb != NULL) || cfg->wdog_en) {
            adc_enable_instance_irq(inst);
        }
        return 0;
    }

    if (ai->mode == INTF_ADC_MODE_SEQ) {
        if (cfg->seq_ch_count == 0 || cfg->seq_ch_count > ADC_SEQ_MAX_LEN)
            return -1;
        if (cfg->dma_en && (cfg->dma_buff == NULL || cfg->dma_buff_len == 0))
            return -1;

        ai->seq.hw_trig = cfg->seq_hw_trig;
        ai->seq.ch_count = cfg->seq_ch_count;
        ai->seq.cb = cfg->seq_cb;
        ai->seq.cb_user_data = cfg->seq_cb_user_data;

        for (uint8_t i = 0; i < cfg->seq_ch_count; i++) {
            ai->seq.ch_list[i] = cfg->seq_ch_list[i];
        }

        adc16_channel_config_t ch_cfg;
        adc16_get_channel_default_config(&ch_cfg);
        ch_cfg.sample_cycle = sample_cycle;

        for (uint8_t i = 0; i < cfg->seq_ch_count; i++) {
            ch_cfg.ch = cfg->seq_ch_list[i];
            if (adc16_init_channel(ai->base, &ch_cfg) != status_success)
                return -1;
            ai->channels[cfg->seq_ch_list[i]].configured = true;
        }

        adc16_seq_config_t seq_cfg;
        seq_cfg.seq_len = cfg->seq_ch_count;
        seq_cfg.restart_en = false;
        seq_cfg.cont_en = true;
        seq_cfg.hw_trig_en = cfg->seq_hw_trig;
        seq_cfg.sw_trig_en = !cfg->seq_hw_trig;

        for (uint8_t i = 0; i < cfg->seq_ch_count; i++) {
            seq_cfg.queue[i].seq_int_en = ((i + 1) == cfg->seq_ch_count);
            seq_cfg.queue[i].ch = cfg->seq_ch_list[i];
        }

        if (adc16_set_seq_config(ai->base, &seq_cfg) != status_success)
            return -1;

        if (cfg->dma_en) {
            ai->dma.active = true;
            ai->dma.buff = cfg->dma_buff;
            ai->dma.len = cfg->dma_buff_len;

            adc16_dma_config_t dma_cfg;
            dma_cfg.start_addr = cfg->dma_buff;
            dma_cfg.buff_len_in_4bytes = cfg->dma_buff_len;
            dma_cfg.stop_en = false;
            dma_cfg.stop_pos = 0;

            if (adc16_init_seq_dma(ai->base, &dma_cfg) != status_success)
                return -1;
        }

        /* 无回调（仅轮询 PRD_RESULT）时不使能中断，避免无谓 ISR；
         * 有回调时仅使能整帧完成中断（回调按帧触发一次） */
        if (cfg->seq_cb != NULL) {
            adc16_enable_interrupts(ai->base, adc16_event_seq_full_complete);
            adc_enable_instance_irq(inst);
        }
        return 0;
    }

    /* Oneshot / Period */
    adc16_channel_config_t ch_cfg;
    adc16_get_channel_default_config(&ch_cfg);
    ch_cfg.ch = ch_idx;
    ch_cfg.sample_cycle = sample_cycle;

    if (cfg->wdog_en) {
        ch_cfg.wdog_int_en = true;
        ch_cfg.thshdh = cfg->wdog_thshd_high;
        ch_cfg.thshdl = cfg->wdog_thshd_low;
    }

    if (adc16_init_channel(ai->base, &ch_cfg) != status_success)
        return -1;

    if (ai->mode == INTF_ADC_MODE_ONESHOT || ai->mode == INTF_ADC_MODE_PERIOD) {
#if defined(ADC_SOC_BUSMODE_ENABLE_CTRL_SUPPORT) && ADC_SOC_BUSMODE_ENABLE_CTRL_SUPPORT
        /* BUS_MODE_EN：BUS_RESULT/PRD_RESULT 结果通路使能 */
        adc16_enable_oneshot_mode(ai->base);
#endif
    }

    ai->channels[ch_idx].configured = true;
    ai->channels[ch_idx].running = false;

    if (cfg->wdog_en) {
        ai->wdog.enabled[ch_idx] = true;
        ai->wdog.cb = cfg->wdog_cb;
        ai->wdog.cb_user_data = cfg->wdog_cb_user_data;
        adc16_enable_interrupts(ai->base, (uint32_t)(1u << ch_idx));
        adc_enable_instance_irq(inst);
    }

    return 0;
}

/* 读取模式（BUS_RESULT）单次读取：WAIT_DIS=1 时 VALID 位是握手标志——
 * 读操作在 VALID=0 时触发一次转换，转换完成后 VALID=1，此时读到的才是本次结果。
 * 重试间隔约 1µs（@480MHz），避免在转换进行中反复触发导致转换永不完成。 */
/**
 * @brief 读取模式单次读取（VALID 握手 + 重试）
 * @param base ADC 基地址
 * @param ch_idx 通道索引
 * @param value 结果输出
 * @return SDK 状态
 */
static hpm_stat_t adc_oneshot_read_retry(ADC16_Type* base, uint8_t ch_idx, uint16_t* value) {
    hpm_stat_t stat = adc16_get_oneshot_result(base, ch_idx, value);
    uint32_t retry = 512U;

    while ((stat != status_success) && (retry-- != 0U)) {
        for (volatile uint32_t delay = 0U; delay < 480U; delay++) {
            /* 等待在途转换完成（约 1µs） */
        }
        stat = adc16_get_oneshot_result(base, ch_idx, value);
    }

    return stat;
}

/**
 * @brief 读取通道原始值（按模式选择结果通路）
 * @param ch 逻辑通道
 * @param value 原始值输出
 * @return 0 = 成功；-1 = 参数非法或未配置
 */
static int adc_read(intf_adc_ch_t ch, uint16_t* value) {
    uint8_t inst = INTF_ADC_CH_INST(ch);
    uint8_t ch_idx = INTF_ADC_CH_IDX(ch);

    if (inst >= INTF_ADC_INSTANCE_COUNT || ch_idx >= ADC_MAX_CHANNELS || value == NULL)
        return -1;

    adc_inst_t* ai = &s_adc_instances[inst];
    if (!ai->initialized || !ai->channels[ch_idx].configured)
        return -1;

    hpm_stat_t stat;
    switch (ai->mode) {
    case INTF_ADC_MODE_PERIOD: stat = adc16_get_prd_result(ai->base, ch_idx, value); break;
    case INTF_ADC_MODE_ONESHOT: stat = adc_oneshot_read_retry(ai->base, ch_idx, value); break;
    case INTF_ADC_MODE_SEQ:
        /* 序列转换结果由硬件同步到 PRD_RESULTx（手册 §53.2.4），纯寄存器读取 */
        stat = adc16_get_prd_result(ai->base, ch_idx, value);
        break;
    case INTF_ADC_MODE_PMT: {
        /* PMT 队列内通道：结果由硬件 DMA 每帧刷新，直接取 DMA 缓冲最近一帧
         * （取最后一个匹配槽：双份队尾方案中尾槽为新鲜值）。 */
        int slot = -1;
        for (uint8_t i = 0U; i < ai->pmt.ch_count; i++) {
            if (ai->pmt.ch_list[i] == ch_idx) {
                slot = (int) i;
            }
        }
        if (slot >= 0) {
            if (!ai->dma.active) {
                return -1;
            }
            volatile uint32_t* dma_hw =
                &ai->dma.buff[(uint32_t) ai->pmt.trig_ch * ADC_PMT_DMA_SLOT_LEN + (uint32_t) slot];
            *value = (uint16_t) (*dma_hw & 0xFFFFU);
            return 0;
        }
        /* 非队列通道（静态慢变量，如 CANID DIP）：读取模式单次读取 */
        if (!ai->channels[ch_idx].configured) {
            adc16_channel_config_t ch_cfg;
            adc16_get_channel_default_config(&ch_cfg);
            ch_cfg.ch = ch_idx;
            ch_cfg.sample_cycle = ADC_DEFAULT_SAMPLE_CYCLE;
            if (adc16_init_channel(ai->base, &ch_cfg) != status_success) {
                return -1;
            }
            ai->channels[ch_idx].configured = true;
        }
        stat = adc_oneshot_read_retry(ai->base, ch_idx, value);
        break;
    }
    default: stat = adc_oneshot_read_retry(ai->base, ch_idx, value); break;
    }

    return (stat == status_success) ? 0 : -1;
}

/**
 * @brief 读取通道电压 [mV]
 * @param ch 逻辑通道
 * @param voltage_mv 电压输出 [mV]
 * @return 0 = 成功；-1 = 参数非法
 */
static int adc_read_voltage(intf_adc_ch_t ch, float* voltage_mv) {
    uint16_t raw;
    if (adc_read(ch, &raw) != 0)
        return -1;

    uint8_t inst = INTF_ADC_CH_INST(ch);
    if (inst >= INTF_ADC_INSTANCE_COUNT || !s_adc_instances[inst].initialized || voltage_mv == NULL)
        return -1;

    intf_adc_resolution_t res = s_adc_instances[inst].resolution;
    uint16_t max_val = adc_resolution_max_value(res);
    float vref = s_adc_instances[inst].vref_mv;
    *voltage_mv = (float)raw * vref / (float)max_val;

    return 0;
}

/**
 * @brief 启动通道转换
 * @param ch 逻辑通道
 * @return 0 = 成功；-1 = 参数非法或未配置
 */
static int adc_start(intf_adc_ch_t ch) {
    uint8_t inst = INTF_ADC_CH_INST(ch);
    uint8_t ch_idx = INTF_ADC_CH_IDX(ch);

    if (inst >= INTF_ADC_INSTANCE_COUNT || ch_idx >= ADC_MAX_CHANNELS)
        return -1;

    adc_inst_t* ai = &s_adc_instances[inst];
    if (!ai->initialized || !ai->channels[ch_idx].configured)
        return -1;

    if (ai->mode == INTF_ADC_MODE_SEQ) {
        if (ai->seq.hw_trig) {
            adc16_seq_enable_hw_trigger(ai->base);
        } else {
            adc16_trigger_seq_by_sw(ai->base);
        }
        return 0;
    }

    if (ai->mode == INTF_ADC_MODE_PMT || ai->mode == INTF_ADC_MODE_ONESHOT) {
        ai->channels[ch_idx].running = true;
        return 0;
    }

    /* 周期 = PRD_CNT × 2^PRESCALE 个 ADC 时钟（PRD_CNT ≤ 255，PRESCALE ≤ 31） */
    {
        uint32_t div = (uint32_t) ADC16_CONV_CFG1_CLOCK_DIVIDER_GET(ai->base->CONV_CFG1) + 1U;
        uint32_t adc_clk = clock_get_frequency(adc_get_clock(inst)) / div;
        uint32_t rate = (ai->period_rate_hz != 0U) ? ai->period_rate_hz : 1000U;
        uint32_t clocks = adc_clk / rate;
        uint8_t prescale = 0U;
        uint32_t cnt;

        if (clocks < 2U) {
            clocks = 2U;
        }
        while (((clocks >> prescale) > 255U) && (prescale < 31U)) {
            prescale++;
        }
        cnt = clocks >> prescale;
        if (cnt > 255U) {
            cnt = 255U;
        }
        if (cnt == 0U) {
            cnt = 1U;
        }

        adc16_prd_config_t prd_cfg;
        prd_cfg.ch = ch_idx;
        prd_cfg.prescale = prescale;
        prd_cfg.period_count = (uint8_t) cnt;
        if (adc16_set_prd_config(ai->base, &prd_cfg) != status_success)
            return -1;

#if defined(ADC_SOC_BUSMODE_ENABLE_CTRL_SUPPORT) && ADC_SOC_BUSMODE_ENABLE_CTRL_SUPPORT
        /* set_prd_config 会恢复 BUF_CFG0，这里重新确保结果通路使能 */
        adc16_enable_oneshot_mode(ai->base);
#endif
    }

    ai->channels[ch_idx].running = true;
    return 0;
}

/**
 * @brief 停止通道转换
 * @param ch 逻辑通道
 * @return 0 = 成功；-1 = 参数非法或未初始化
 */
static int adc_stop(intf_adc_ch_t ch) {
    uint8_t inst = INTF_ADC_CH_INST(ch);
    uint8_t ch_idx = INTF_ADC_CH_IDX(ch);

    if (inst >= INTF_ADC_INSTANCE_COUNT || ch_idx >= ADC_MAX_CHANNELS)
        return -1;

    adc_inst_t* ai = &s_adc_instances[inst];
    if (!ai->initialized)
        return -1;

    if (ai->mode == INTF_ADC_MODE_SEQ) {
        adc16_seq_disable_hw_trigger(ai->base);
        return 0;
    }

    ai->channels[ch_idx].running = false;
    return 0;
}

/* ============================================================================
 * Operations Structures & Registration
 * ============================================================================ */

static const intf_adc_t s_adc_ops_adc0 = {
    .instance_id = 0,
    .init = adc_init,
    .read = adc_read,
    .read_voltage = adc_read_voltage,
    .start = adc_start,
    .stop = adc_stop,
};

static const intf_adc_t s_adc_ops_adc1 = {
    .instance_id = 1,
    .init = adc_init,
    .read = adc_read,
    .read_voltage = adc_read_voltage,
    .start = adc_start,
    .stop = adc_stop,
};

void hpm_adc_driver_register(void) {
    intf_adc_register(&s_adc_ops_adc0);
    intf_adc_register(&s_adc_ops_adc1);
}
