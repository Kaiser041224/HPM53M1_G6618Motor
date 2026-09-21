/**
 * @file    intf_adc.h
 * @brief   ADC 抽象接口（多实例）
 * @author  Kaiser
 *
 * HPM5361 有 2 个 ADC16 实例（ADC0 / ADC1），各最多 16 通道。通道号编码：
 *   bits [7:4] = 实例（0 → ADC0，1 → ADC1）
 *   bits [3:0] = 物理通道号（0–15）
 *
 * 用法：INTF_ADC_CH(0, 3) → ADC0 channel 3
 *       INTF_ADC_CH(1, 7) → ADC1 channel 7
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _INTF_ADC_H
#define _INTF_ADC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ADC 通道编码（实例 + 物理通道）
 */
typedef uint8_t intf_adc_ch_t;

#define INTF_ADC_CH(inst, idx)  ((intf_adc_ch_t)(((uint8_t)(inst) << 4) | ((uint8_t)(idx) & 0x0FU))) /**< 组合实例与通道号 */
#define INTF_ADC_CH_INST(ch)    ((uint8_t)((ch) >> 4))                                                 /**< 提取实例号 */
#define INTF_ADC_CH_IDX(ch)     ((uint8_t)((ch) & 0x0FU))                                              /**< 提取物理通道号 */
#define INTF_ADC_INSTANCE_COUNT (2U)                                                                   /**< ADC 实例数 */

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * @brief ADC 分辨率选项
 */
typedef enum {
    INTF_ADC_RES_8_BITS  = 8,  /**< 8 位 */
    INTF_ADC_RES_10_BITS = 10, /**< 10 位 */
    INTF_ADC_RES_12_BITS = 12, /**< 12 位 */
    INTF_ADC_RES_16_BITS = 16, /**< 16 位 */
} intf_adc_resolution_t;

#define INTF_ADC_RES_DEFAULT INTF_ADC_RES_16_BITS /**< 默认分辨率 */

/* Configurable defaults (0 in cfg = use these values) */
#define INTF_ADC_DEFAULT_SAMPLE_CYCLE (25U)      /**< 默认采样周期数 */
#define INTF_ADC_DEFAULT_CLOCK_DIV    (4U)       /**< 默认时钟分频（AHB 160 MHz / 4 = 40 MHz ≤ 50 MHz） */
#define INTF_ADC_DEFAULT_VREF_MV      (3300.0f)  /**< 默认参考电压 [mV] */

/**
 * @brief ADC 转换模式
 */
typedef enum {
    INTF_ADC_MODE_ONESHOT = 0, /**< 单次转换 */
    INTF_ADC_MODE_PERIOD  = 1, /**< 周期转换 */
    INTF_ADC_MODE_PMT     = 2, /**< 抢占（PMT）模式 */
    INTF_ADC_MODE_SEQ     = 3, /**< 序列模式 */
} intf_adc_mode_t;

/**
 * @brief PMT 触发完成回调
 * @param trig_ch 触发通道
 * @param values 采样值数组
 * @param count 采样值个数
 * @param user_data 用户数据
 */
typedef void (*intf_adc_pmt_cb_t)(
    intf_adc_ch_t trig_ch, const uint16_t* values, uint8_t count, void* user_data);

/**
 * @brief 序列模式完成回调（DMA 缓冲就绪）
 * @param trig_ch 触发通道
 * @param user_data 用户数据
 */
typedef void (*intf_adc_seq_cb_t)(intf_adc_ch_t trig_ch, void* user_data);

/**
 * @brief 看门狗阈值越限回调
 * @param ch 越限通道
 * @param value 越限采样值
 * @param user_data 用户数据
 */
typedef void (*intf_adc_wdog_cb_t)(intf_adc_ch_t ch, uint16_t value, void* user_data);

/**
 * @brief ADC 诊断快照
 */
typedef struct {
    uint32_t irq_entry[INTF_ADC_INSTANCE_COUNT];                  /**< 中断进入次数 */
    uint32_t generic_entry[INTF_ADC_INSTANCE_COUNT];              /**< 通用中断进入次数 */
    uint32_t pmt_complete[INTF_ADC_INSTANCE_COUNT];               /**< PMT 完成次数 */
    uint32_t pmt_startup_drop[INTF_ADC_INSTANCE_COUNT];           /**< PMT 启动期丢弃次数 */
    uint32_t pmt_callback[INTF_ADC_INSTANCE_COUNT];               /**< PMT 回调次数 */
    uint32_t pmt_invalid[INTF_ADC_INSTANCE_COUNT];                /**< PMT 非法帧次数 */
    uint32_t pmt_invalid_cycle[INTF_ADC_INSTANCE_COUNT];          /**< PMT cycle-bit 非法次数 */
    uint32_t pmt_invalid_trig[INTF_ADC_INSTANCE_COUNT];           /**< PMT 触发非法次数 */
    uint32_t pmt_invalid_channel[INTF_ADC_INSTANCE_COUNT];        /**< PMT 通道非法次数 */
    uint32_t isr_cycles_max[INTF_ADC_INSTANCE_COUNT];             /**< ISR 最大周期数 */
    uint64_t isr_total_cycles[INTF_ADC_INSTANCE_COUNT];           /**< 累计 ISR 周期（算占用率；64 位防截断） */
    uint32_t pmt_last[INTF_ADC_INSTANCE_COUNT][4];                /**< 最近一帧 PMT 原始字（取证用） */
    uint32_t pmt_cycle_fallback[INTF_ADC_INSTANCE_COUNT];         /**< cycle-bit 协议回退次数 */
    uint32_t reg_conv_cfg1[INTF_ADC_INSTANCE_COUNT];              /**< 诊断：CONV_CFG1 */
    uint32_t reg_adc_cfg0[INTF_ADC_INSTANCE_COUNT];               /**< 诊断：ADC_CFG0 */
    uint32_t reg_buf_cfg0[INTF_ADC_INSTANCE_COUNT];               /**< 诊断：BUF_CFG0 */
    uint32_t reg_config0[INTF_ADC_INSTANCE_COUNT];                /**< 诊断：CONFIG[TRG0A] */
    uint32_t reg_int_sts[INTF_ADC_INSTANCE_COUNT];                /**< 诊断：INT_STS */
    uint32_t reg_seq_cfg0[INTF_ADC_INSTANCE_COUNT];               /**< 诊断：SEQ_CFG0 */
    uint32_t reg_prd_result[INTF_ADC_INSTANCE_COUNT][16];         /**< 诊断：PRD_RESULT[0..15]（所有模式结果同步寄存器） */
} intf_adc_diag_snapshot_t;

/**
 * @brief 单实例 ADC 配置
 */
typedef struct {
    intf_adc_resolution_t resolution;       /**< 分辨率 */
    intf_adc_mode_t mode;                   /**< 转换模式 */
    uint32_t sample_rate_hz;                /**< 目标采样率（Period 模式），或 0 用默认 */
    uint32_t sample_cycle;                  /**< 每通道采样周期数（0 = INTF_ADC_DEFAULT_SAMPLE_CYCLE） */
    uint32_t clock_div;                     /**< ADC 时钟分频 1–16（0 = 由 sample_rate_hz 自动） */
    float vref_mv;                          /**< 参考电压 [mV] */
    /* DMA (applicable to PMT and Sequence modes) */
    bool dma_en;                            /**< 使能 DMA（PMT / Sequence 模式） */
    uint32_t* dma_buff;                     /**< DMA 缓冲区 */
    uint32_t dma_buff_len;                  /**< DMA 缓冲区长度 */
    /* PMT mode */
    uint8_t pmt_trig_ch;                    /**< PMT 触发通道 */
    uint8_t pmt_ch_count;                   /**< PMT 通道数 */
    uint8_t pmt_ch_list[4];                 /**< PMT 通道列表 */
    intf_adc_pmt_cb_t pmt_cb;               /**< PMT 完成回调 */
    void* pmt_cb_user_data;                 /**< PMT 回调用户数据 */
    /* Sequence mode */
    bool seq_hw_trig;                       /**< 序列模式：硬件触发 */
    uint8_t seq_ch_count;                   /**< 序列通道数 */
    uint8_t seq_ch_list[16];                /**< 序列通道列表 */
    intf_adc_seq_cb_t seq_cb;               /**< 序列完成回调 */
    void* seq_cb_user_data;                 /**< 序列回调用户数据 */
    /* Watchdog */
    bool wdog_en;                           /**< 使能看门狗 */
    uint16_t wdog_thshd_high;               /**< 看门狗上限阈值 */
    uint16_t wdog_thshd_low;                /**< 看门狗下限阈值 */
    intf_adc_wdog_cb_t wdog_cb;             /**< 看门狗回调 */
    void* wdog_cb_user_data;                /**< 看门狗回调用户数据 */
} intf_adc_cfg_t;

/* ============================================================================
 * Interface Definition (Object-Oriented C17)
 * ============================================================================ */

/**
 * @brief ADC 抽象接口
 */
typedef struct {
    uint8_t instance_id; /**< 实例编号 */
    struct {
        /**
         * @brief 初始化通道
         * @param ch 通道
         * @param cfg 通道配置
         * @return 0 = 成功；-1 = 失败
         */
        int (*init)(intf_adc_ch_t ch, const intf_adc_cfg_t* cfg);

        /**
         * @brief 读取原始采样值
         * @param ch 通道
         * @param value 输出原始值
         * @return 0 = 成功；-1 = 失败
         */
        int (*read)(intf_adc_ch_t ch, uint16_t* value);

        /**
         * @brief 读取电压值
         * @param ch 通道
         * @param voltage_mv 输出电压 [mV]
         * @return 0 = 成功；-1 = 失败
         */
        int (*read_voltage)(intf_adc_ch_t ch, float* voltage_mv);

        /**
         * @brief 启动通道转换
         * @param ch 通道
         * @return 0 = 成功；-1 = 失败
         */
        int (*start)(intf_adc_ch_t ch);

        /**
         * @brief 停止通道转换
         * @param ch 通道
         * @return 0 = 成功；-1 = 失败
         */
        int (*stop)(intf_adc_ch_t ch);
    };
} intf_adc_t;

/* ============================================================================
 * Registration API
 * ============================================================================ */

/**
 * @brief 注册 ADC 接口实现
 * @param ops 接口实现
 * @return 0 = 成功；-1 = 失败
 */
int intf_adc_register(const intf_adc_t* ops);

/* ============================================================================
 * Functional API (wraps ops)
 * ============================================================================ */

/**
 * @brief 初始化通道
 * @param ch 通道
 * @param cfg 通道配置
 * @return 0 = 成功；-1 = 失败
 */
int intf_adc_init(intf_adc_ch_t ch, const intf_adc_cfg_t* cfg);

/**
 * @brief 读取原始采样值
 * @param ch 通道
 * @param value 输出原始值
 * @return 0 = 成功；-1 = 失败
 */
int intf_adc_read(intf_adc_ch_t ch, uint16_t* value);

/**
 * @brief 读取电压值
 * @param ch 通道
 * @param voltage_mv 输出电压 [mV]
 * @return 0 = 成功；-1 = 失败
 */
int intf_adc_read_voltage(intf_adc_ch_t ch, float* voltage_mv);

/**
 * @brief 启动通道转换
 * @param ch 通道
 * @return 0 = 成功；-1 = 失败
 */
int intf_adc_start(intf_adc_ch_t ch);

/**
 * @brief 停止通道转换
 * @param ch 通道
 * @return 0 = 成功；-1 = 失败
 */
int intf_adc_stop(intf_adc_ch_t ch);

/**
 * @brief 读取诊断快照
 * @param snapshot 输出诊断快照
 * @return 0 = 成功；-1 = 失败
 */
int intf_adc_get_diag_snapshot(intf_adc_diag_snapshot_t* snapshot);

/**
 * @brief 清零诊断最大值
 */
void intf_adc_reset_diag_max(void);

/* WDOG re-arm: re-arm the interrupt for a channel after wdog_cb fired.
 * Required because the ISR auto-disables the channel's WDOG interrupt
 * to avoid flooding. */

/**
 * @brief 重新武装看门狗中断（回调触发后）
 * @param ch 通道
 */
void intf_adc_wdog_reenable(intf_adc_ch_t ch);

#ifdef __cplusplus
}
#endif

#endif /* _INTF_ADC_H */
