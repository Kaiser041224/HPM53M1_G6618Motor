/**
 * @file    app_adc.h
 * @brief   M1 采样链（PWM1 触发 → TRGM → 双 ADC PMT）
 * @author  Kaiser
 *
 * 采样链（2026-09-19 定稿）：
 *   PWM1 CMP10（计数谷底 + trigger_delay_ns）
 *     → CHCFG[10] → PWM1_CH10REF → TRGM0 PTRGI0A
 *     → ADC0 PMT：I_W(IN2) → I_U(IN3) → I_V(IN4) → I_W(IN2)   电流（首槽=队尾副本）
 *   GPTMR0 CH2（1kHz 方波）→ TRGM0 → ADC1_STRGI
 *     → ADC1 SEQ：CANID(IN15) → V_VBUS(IN6) → NTC0(IN11) → NTC1(IN1) → CANID(IN15)
 *                 慢速（首项=队尾副本），结果同步到 PRD_RESULTx
 *   ADC0 结果由内部 DMA 写入缓冲、ISR 回调刷新 raw 缓存；
 *   ADC1 结果由 1kHz 慢任务从 PRD_RESULTx 纯寄存器读取（无中断）。
 *
 * 时序（按当前默认配置：25kHz / MOT 160MHz / ADC 40MHz，16bit + sample_cycle=25）：
 *   单通道 ≈1.15µs（25 采样 + 21 转换 个 ADC 时钟）；4 槽队列 ≈4.6µs；
 *   低侧导通窗口半宽 = (1−占空比)×20µs（更高调制区由后续 FOC 用两相重构处理）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_ADC_H
#define APP_ADC_H

#include "intf_adc.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 逻辑通道（与 app_adc.c 映射表一一对应）
 */
typedef enum {
    ADC_CH_I_U = 0, /**< ADC0_IN3  PB11  U 相电流（低侧 2mΩ + TPA6584Q ×7.5） */
    ADC_CH_I_V,     /**< ADC0_IN4  PB12  V 相电流 */
    ADC_CH_I_W,     /**< ADC0_IN2  PB10  W 相电流 */
    ADC_CH_V_VBUS,  /**< ADC1_IN6  PB14  母线电压（73.3K/3.3K 分压 + 内部运放 B） */
    ADC_CH_NTC0,    /**< ADC1_IN11 PB08  NTC0（10K 上拉，温度换算待型号确定） */
    ADC_CH_NTC1,    /**< ADC1_IN1  PB09  NTC1 */
    ADC_CH_V_CANID, /**< ADC1_IN15 PB00  CANID DIP 电阻网络（随 ADC1 1kHz 序列采样） */
    ADC_CH_COUNT,
} adc_channel_t;

/* CANID 为静态慢变量：随 ADC1 序列（1kHz）一起采样 */
_Static_assert(ADC_CH_V_CANID == (ADC_CH_COUNT - 1), "CANID must be the last channel");

/* 默认配置来源：config/hardware.yaml（app_hardware_params.adc） */
#define APP_ADC_TRIGGER_CMP_INDEX (10U) /* PWM1 比较器/输出通道（三相占用 0/1、8/9、12/13） */

/* WDOG 回调（逻辑通道，非硬件通道） */
typedef void (*app_adc_wdog_cb_t)(adc_channel_t ch, uint16_t value, void* user);

/** PMT 完成回调（FOC 快车道钩子；ISR 上下文，25kHz 硬件触发） */
typedef void (*app_adc_fast_cb_t)(void* user);

/**
 * @brief ADC 采样链初始化配置
 */
typedef struct {
    uint32_t trigger_delay_ns; /**< 谷底后触发延时 [ns]（0 = 回退 hardware.adc.trigger_delay_ns） */
    uint16_t sample_cycle;     /**< 采样窗口 [ADC 时钟数]（0 = 回退 hardware.adc.sample_cycle） */
    uint8_t resolution;        /**< intf_adc_resolution_t（0 = 默认 16bit） */
    /* ADC0 电流通道 WDOG（硬件阈值；wdog_en=false 时忽略） */
    bool wdog_en;              /**< WDOG 使能 */
    uint16_t wdog_thshd_high;  /**< WDOG 高阈值原始码 */
    uint16_t wdog_thshd_low;   /**< WDOG 低阈值原始码 */
    app_adc_wdog_cb_t wdog_cb; /**< WDOG 回调 */
    void* wdog_cb_user;        /**< WDOG 回调用户上下文 */
    /* ADC0 PMT 完成回调（25kHz，ISR 上下文；NULL = 不启用） */
    app_adc_fast_cb_t fast_cb; /**< FOC 快车道钩子（相电流收取后调用） */
    void* fast_cb_user;        /**< 快车道钩子用户上下文 */
    /* ADC1 序列完成回调（1kHz，ISR 上下文；NULL = 不启用） */
    void (*slow_cb)(void); /**< 慢速序列完成回调 */
} app_adc_cfg_t;

/**
 * @brief 初始化采样链：双 ADC PMT + TRGM 路由 + PWM1 触发比较器，
 *        并启动 PWM1 计数器（仅计数、输出保持关闭；PMT 触发依赖计数器运行）。
 * @note 应在 app_hrpwm/逆变桥初始化之后调用（需要 PWM 时钟与实例信息）。
 * @param cfg 配置；NULL = 默认（trigger_delay/sample_cycle 取自
 * config/hardware.yaml；resolution=16bit）
 */
void app_adc_init(const app_adc_cfg_t* cfg);

/** @brief 实际生效配置（调试打印 / 参数核对用） */
const app_adc_cfg_t* app_adc_get_config(void);

/**
 * @brief 读取通道最新原始值（ISR 缓存）。
 * @param ch 逻辑通道
 * @param raw 输出原始码
 * @return true = 该通道已有有效数据
 */
bool app_adc_get_raw(adc_channel_t ch, uint16_t* raw);

/**
 * @brief 电流帧序号（ADC0 每完成一帧 +1），用于判断新数据
 * @return 当前帧序号
 */
uint32_t app_adc_get_sequence(void);

/**
 * @brief 慢速通道采样（建议 1kHz 调用）。
 *        覆盖 V_VBUS / NTC0 / NTC1 / V_CANID：直接读取 ADC1 序列转换的
 *        PRD_RESULTx（GPTMR0 1kHz 硬件触发，每轮 5 项），写入 raw 缓存。
 */
void app_adc_slow_process(void);

/**
 * @brief 重装指定通道的 WDOG 中断（故障清除后调用）
 * @param ch 逻辑通道
 */
void app_adc_wdog_reenable(adc_channel_t ch);

/**
 * @brief 全部通道是否均已产出数据
 * @return true = 全部通道已有有效数据
 */
bool app_adc_is_valid(void);

/**
 * @brief 原始码 → 电压 [V]（按当前分辨率与参考电压换算）
 * @param code 原始码
 * @return 换算电压 [V]
 */
float app_adc_code_to_volts(uint16_t code);

/**
 * @brief 运行时更新触发延时（调试期扫描采样点用）。
 * @param delay_ns 触发延时 [ns]
 * @return 0 成功，-1 失败
 */
int app_adc_set_trigger_delay_ns(uint32_t delay_ns);

#ifdef __cplusplus
}
#endif

#endif /* APP_ADC_H */
