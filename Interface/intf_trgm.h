/**
 * @file    intf_trgm.h
 * @brief   TRGM 触发矩阵抽象接口（多实例）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef INTF_TRGM_H
#define INTF_TRGM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Trigger Source Enumeration
 *
 * HPM5361 supports TRGM0/1 instances. Current usage routes PWM compare
 * reference outputs to ADC preemption trigger inputs (PTRGIxA/xB/xC).
 * ============================================================================ */

/**
 * @brief TRGM 触发源
 */
typedef enum {
    INTF_TRGM_SRC_PWM0_CH8REF = 0,       /**< PWM0 CH8 比较输出 */
    INTF_TRGM_SRC_PWM0_CH9REF,           /**< PWM0 CH9 比较输出 */
    INTF_TRGM_SRC_PWM0_CH10REF,          /**< PWM0 CH10 比较输出 */
    INTF_TRGM_SRC_PWM0_CH11REF,          /**< PWM0 CH11 比较输出 */
    INTF_TRGM_SRC_PWM1_CH8REF,           /**< PWM1 CH8 比较输出 */
    INTF_TRGM_SRC_PWM1_CH9REF,           /**< PWM1 CH9 比较输出 */
    INTF_TRGM_SRC_PWM1_CH10REF,          /**< PWM1 CH10 比较输出 */
    INTF_TRGM_SRC_PWM1_CH11REF,          /**< PWM1 CH11 比较输出 */
    INTF_TRGM_SRC_SYNT_CH0,              /**< SYNT CH0 比较输出 */
    INTF_TRGM_SRC_SYNT_CH1,              /**< SYNT CH1 比较输出 */
    INTF_TRGM_SRC_SYNT_CH2,              /**< SYNT CH2 比较输出 */
    INTF_TRGM_SRC_SYNT_CH3,              /**< SYNT CH3 比较输出 */
    INTF_TRGM_SRC_GPTMR0_OUT2,           /**< GPTMR0 CH2 比较输出（1kHz 慢速触发源） */
    INTF_TRGM_SRC_GPTMR0_OUT3,           /**< GPTMR0 CH3 比较输出 */
} intf_trgm_src_t;

/* ============================================================================
 * Trigger Destination Enumeration
 * ============================================================================ */

/**
 * @brief TRGM 触发目标
 */
typedef enum {
    INTF_TRGM_DST_ADC_PTRGI0A = 0,       /**< → ADC TRG0A (pmt_trig_ch=0) */
    INTF_TRGM_DST_ADC_PTRGI0B,           /**< → ADC TRG0B (pmt_trig_ch=1) */
    INTF_TRGM_DST_ADC_PTRGI0C,           /**< → ADC TRG0C (pmt_trig_ch=2) */
    INTF_TRGM_DST_ADC_PTRGI1A,           /**< → ADC TRG1A (pmt_trig_ch=3) */
    INTF_TRGM_DST_ADC_PTRGI1B,           /**< → ADC TRG1B (pmt_trig_ch=4) */
    INTF_TRGM_DST_ADC_PTRGI1C,           /**< → ADC TRG1C (pmt_trig_ch=5) */
    INTF_TRGM_DST_GPTMR0_SYNCI,          /**< → GPTMR0 counter sync input */
    INTF_TRGM_DST_GPTMR1_SYNCI,          /**< → GPTMR1 counter sync input */
    INTF_TRGM_DST_GPTMR2_SYNCI,          /**< → GPTMR2 counter sync input */
    INTF_TRGM_DST_GPTMR3_SYNCI,          /**< → GPTMR3 counter sync input */
    INTF_TRGM_DST_ADC0_STRGI,            /**< → ADC0 序列转换触发输入 */
    INTF_TRGM_DST_ADC1_STRGI,            /**< → ADC1 序列转换触发输入 */
} intf_trgm_dst_t;

/* ============================================================================
 * Interface Definition (Object-Oriented C17)
 * ============================================================================ */

/**
 * @brief TRGM 抽象接口
 */
typedef struct {
    uint8_t instance_id; /**< 实例编号 */
    struct {
        /**
         * @brief 连接触发源到触发目标
         * @param src 触发源
         * @param dst 触发目标
         * @return 0 = 成功；-1 = 失败
         */
        int (*connect)(intf_trgm_src_t src, intf_trgm_dst_t dst);
    };
} intf_trgm_t;

/* ============================================================================
 * Registration API
 * ============================================================================ */

/**
 * @brief 注册 TRGM 接口实现
 * @param ops 接口实现
 * @return 0 = 成功；-1 = 失败
 */
int intf_trgm_register(const intf_trgm_t *ops);

/* ============================================================================
 * Functional API (wraps ops)
 * ============================================================================ */

/**
 * @brief 连接触发源到触发目标
 * @param src 触发源
 * @param dst 触发目标
 * @return 0 = 成功；-1 = 失败
 */
int intf_trgm_connect(intf_trgm_src_t src, intf_trgm_dst_t dst);

#ifdef __cplusplus
}
#endif

#endif /* INTF_TRGM_H */
