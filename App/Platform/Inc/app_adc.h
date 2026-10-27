/**
 * @file    app_adc.h
 * @brief   M1 采样链（PWM1 触发 → TRGM → 双 ADC PMT）
 * @author  Kaiser
 *
 * 采样链（2026-09-19 定稿；2026-10-26 vbus 同源同步修订）：
 *   PWM1 CMP10（计数谷底 + trigger_delay_ns）→ CHCFG[10] → PWM1_CH10REF
 *     ├─→ TRGM0 PTRGI0A → ADC0 PMT：I_W(IN2) → I_U(IN3) → I_V(IN4) → I_W(IN2)
 *     │                     电流（首槽=队尾副本）；DMA → ISR 刷新 raw 缓存 + FOC 电流环
 *     │                     + **vbus 快通道锁存**（同拍读 ADC1 结果寄存器）
 *     └─→ TRGM0 ADC1_STRGI → ADC1 SEQ：V_VBUS(IN6) → NTC0(IN11) → NTC1(IN1)
 *                             → CANID(IN15) → V_VBUS(IN6)（首末同通道=自吸收）
 *            结果同步到 PRD_RESULTx；1kHz 慢任务纯寄存器读取（NTC/CANID/1kHz vbus）
 *            SEQ 完成 ISR 只承载保护 fault tick，并按 pwm_freq/1000 分频保持 1kHz
 *
 * 关键不变量（改动前必读）：
 *   · PMT 每触发 ≤4 槽（ADC_SOC_MAX_TRIG_CH_LEN=4）；ADC0 队列已满 → vbus 无法进同队列
 *   · 两个队列的首槽都会被"上一帧最后一个通道"污染 → 首槽通道必须 == 队尾通道
 *   · vbus 置于 SEQ 首项：其转换早于 ADC0 PMT 完成，故 PMT ISR 同拍可读到本帧值
 *   · 不要缩短 SEQ 序列：5 项的完成时刻(≈5.8µs)晚于 PMT(≈4.6µs)，在不可抢占 ISR 构建下
 *     这保证 ADC1 的 IRQ 总排在 FOC ISR 之后（缩短会让 ADC1 先跑并阻塞控制环）
 *
 * 时序（按当前默认配置：30kHz / MOT 160MHz / ADC 40MHz，16bit + sample_cycle=10）：
 *   单通道 ≈1.15µs（25 采样 + 21 转换 个 ADC 时钟）；4 槽队列 ≈4.6µs；SEQ 5 项 ≈5.8µs；
 *   低侧导通窗口半宽 = (1−占空比)×16.7µs（更高调制区由后续 FOC 用两相重构处理）。
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
    ADC_CH_V_CANID, /**< ADC1_IN15 PB00  CANID DIP 电阻网络（随 ADC1 序列采样，读取在 1kHz 任务） */
    ADC_CH_COUNT,
} adc_channel_t;

/* CANID 为静态慢变量：随 ADC1 序列（= pwm_freq）一起转换，但读取在 1kHz 任务 */
_Static_assert(ADC_CH_V_CANID == (ADC_CH_COUNT - 1), "CANID must be the last channel");

/* 默认配置来源：config/hardware.yaml（app_hardware_params.adc） */
#define APP_ADC_TRIGGER_CMP_INDEX (10U) /* PWM1 比较器/输出通道（三相占用 0/1、8/9、12/13） */

/* WDOG 回调（逻辑通道，非硬件通道） */
typedef void (*app_adc_wdog_cb_t)(adc_channel_t ch, uint16_t value, void* user);

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
    /* ADC1 序列完成回调：**目标 1kHz**（ISR 上下文）。ADC1 SEQ 与电流同源触发 =
     * pwm_freq，故实现内部按 pwm_freq/1000 分频后调用，使保护 fault tick 的节拍与
     * 去抖语义（5 次 = 5ms）保持不变。NULL = 不使能 SEQ 完成中断。 */
    void (*slow_cb)(void); /**< 慢速序列完成回调（分频后 1kHz，ISR 上下文） */
} app_adc_cfg_t;

/**
 * @brief 初始化采样链：ADC0 PMT + ADC1 SEQ + TRGM 扇出路由 + PWM1 触发比较器，
 *        并启动 PWM1 计数器（仅计数、输出保持关闭；PMT 触发依赖计数器运行）。
 * @note 应在 app_hrpwm/逆变桥初始化之后调用（需要 PWM 时钟与实例信息）。
 * @param cfg 配置；NULL = 默认（trigger_delay/sample_cycle 取自
 * config/hardware.yaml；resolution=16bit）
 */
void app_adc_init(const app_adc_cfg_t* cfg);

/**
 * @brief 注册"电流环钩子"：在 ADC0 PMT 完成中断内、原始码锁存后立即调用
 * @param hook 回调（ISR 上下文；NULL = 注销）
 * @note 供 FOC 把电流环放进采样完成点，获得固定 25kHz 节拍与最小
 *       "采样→输出"延迟（主循环节拍抖动不再影响控制环）。
 */
void app_adc_register_current_hook(void (*hook)(void));

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
 * @brief 快通道母线原始码（ADC1 SEQ 与电流**同源触发**，PMT ISR 内锁存）
 *
 * vbus 是 ADC1 序列首项 → 触发后 ≈1.15µs 完成，早于 ADC0 PMT（4 槽 ≈4.6µs），
 * 故 ADC0 的 PMT ISR 每拍都能读到"本帧"值；相对电流最大陈旧 1 帧（pwm 周期），
 * 且采样相位与电流完全相同。
 *
 * @param raw 输出：ADC1_IN6 原始码
 * @return true = 已收到首帧且至少有一次有效锁存；false = 尚无（调用方应回退 1kHz 缓存）
 */
bool app_adc_get_vbus_fast(uint16_t* raw);

/** @brief 快通道母线锁存序号（每次有效锁存 +1；供消费者判定"是否为本拍新值"） */
uint32_t app_adc_get_vbus_fast_sequence(void);

/** @brief 快通道母线陈旧拍数（SEQ 帧计数未推进 = ADC1 链未产出新帧） */
uint32_t app_adc_get_vbus_fast_stale_count(void);

/**
 * @brief 慢速通道采样（建议 1kHz 调用）。
 *        覆盖 V_VBUS / NTC0 / NTC1 / V_CANID：直接读取 ADC1 序列转换的
 *        PRD_RESULTx（与电流同源硬件触发 = pwm_freq，每轮 5 项），写入 raw 缓存。
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
