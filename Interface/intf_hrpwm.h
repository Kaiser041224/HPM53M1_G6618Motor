/**
 * @file    intf_hrpwm.h
 * @brief   高性能 PWM（HRPWM）抽象接口
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef INTF_HRPWM_H
#define INTF_HRPWM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HRPWM 实例号
 */
typedef uint8_t intf_hrpwm_inst_t;

/**
 * @brief HRPWM 通道号
 */
typedef uint8_t intf_hrpwm_ch_t;

/**
 * @brief HRPWM 配对号
 */
typedef uint8_t intf_hrpwm_pair_t;

/**
 * @brief PWM 中断回调（中断上下文执行）
 */
typedef void (*intf_hrpwm_irq_callback_t)(void);

/**
 * @brief HRPWM 故障源
 */
typedef enum {
    INTF_HRPWM_FAULT_SRC_INTERNAL_0 = 0, /**< 内部故障源 0 */
    INTF_HRPWM_FAULT_SRC_INTERNAL_1,     /**< 内部故障源 1 */
    INTF_HRPWM_FAULT_SRC_INTERNAL_2,     /**< 内部故障源 2 */
    INTF_HRPWM_FAULT_SRC_INTERNAL_3,     /**< 内部故障源 3 */
    INTF_HRPWM_FAULT_SRC_EXTERNAL_0,     /**< 外部故障源 0 */
    INTF_HRPWM_FAULT_SRC_EXTERNAL_1,     /**< 外部故障源 1 */
    INTF_HRPWM_FAULT_SRC_DEBUG,          /**< 调试故障源 */
} intf_hrpwm_fault_src_t;

/**
 * @brief HRPWM 故障响应方式
 */
typedef enum {
    INTF_HRPWM_FAULT_MODE_FORCE_LOW = 0, /**< 强制低电平 */
    INTF_HRPWM_FAULT_MODE_FORCE_HIGH,    /**< 强制高电平 */
    INTF_HRPWM_FAULT_MODE_HIGH_Z,        /**< 高阻 */
} intf_hrpwm_fault_mode_t;

/**
 * @brief HRPWM 故障恢复方式
 */
typedef enum {
    INTF_HRPWM_FAULT_RECOVERY_IMMEDIATELY = 0, /**< 立即恢复 */
    INTF_HRPWM_FAULT_RECOVERY_ON_RELOAD,       /**< 重装载时恢复 */
    INTF_HRPWM_FAULT_RECOVERY_ON_HW_EVENT,     /**< 硬件事件恢复 */
    INTF_HRPWM_FAULT_RECOVERY_ON_FAULT_CLEAR,  /**< 故障清除后恢复 */
} intf_hrpwm_fault_recovery_t;

/**
 * @brief HRPWM 对齐方式
 */
typedef enum {
    INTF_HRPWM_ALIGN_EDGE = 0, /**< 边沿对齐 */
    INTF_HRPWM_ALIGN_CENTER,   /**< 中心对齐 */
} intf_hrpwm_align_t;

/**
 * @brief HRPWM 配对配置
 */
typedef struct {
    uint32_t           frequency_hz;     /**< 频率 [Hz] */
    float              duty;             /**< 占空比 [0.0, 1.0] */
    uint32_t           deadtime_ns;      /**< 死区 [ns] */
    uint8_t            jitter_cmp;       /**< 抖动比较值 */
    intf_hrpwm_align_t align;            /**< 对齐方式 */
    bool               invert_high_side; /**< 高边输出反相 */
    bool               invert_low_side;  /**< 低边输出反相 */
} intf_hrpwm_pair_cfg_t;

/**
 * @brief HRPWM 故障配置
 */
typedef struct {
    intf_hrpwm_fault_src_t      source;   /**< 故障源 */
    intf_hrpwm_fault_mode_t     mode;     /**< 故障响应方式 */
    intf_hrpwm_fault_recovery_t recovery; /**< 故障恢复方式 */
    bool                        active_low; /**< 故障信号低有效 */
} intf_hrpwm_fault_cfg_t;

/**
 * @brief HRPWM 移相配置
 */
typedef struct {
    intf_hrpwm_inst_t inst;        /**< PWM 实例（0 或 1） */
    intf_hrpwm_pair_t ref_pair;    /**< 参考 pair（0 或 1） */
    intf_hrpwm_pair_t target_pair; /**< 目标 pair（0 或 1） */
    float             phase_deg;   /**< 移相角度（0-max_phase_deg） */
} intf_hrpwm_phase_cfg_t;

/**
 * @brief HRPWM 移相限制配置
 */
typedef struct {
    float max_phase_deg;   /**< 最大移相角度，默认 180.0 */
    float max_duty_ref;    /**< 参考 pair 最大占空比限制，默认 1.0 */
    float max_duty_target; /**< 目标 pair 最大占空比限制，默认 1.0 */
} intf_hrpwm_phase_limit_t;

/**
 * @brief HRPWM 抽象接口
 */
typedef struct {
    uint8_t instance_id; /**< 实例编号 */
    struct {
        /**
         * @brief 初始化配对
         * @param ch 通道号
         * @param cfg 配对配置
         * @return 0 = 成功；-1 = 失败
         */
        int (*init_pair)(intf_hrpwm_ch_t ch, const intf_hrpwm_pair_cfg_t *cfg);

        /**
         * @brief 设置占空比（经约束）
         * @param ch 通道号
         * @param duty 占空比 [0.0, 1.0]
         * @return 0 = 成功；-1 = 失败
         */
        int (*set_duty)(intf_hrpwm_ch_t ch, float duty);

        /**
         * @brief 直接设置占空比（不经约束）
         * @param ch 通道号
         * @param duty 占空比 [0.0, 1.0]
         * @return 0 = 成功；-1 = 失败
         */
        int (*set_duty_direct)(intf_hrpwm_ch_t ch, float duty);

        /**
         * @brief 同时直接设置两通道占空比
         * @param ch_a 通道 A
         * @param duty_a 通道 A 占空比 [0.0, 1.0]
         * @param ch_b 通道 B
         * @param duty_b 通道 B 占空比 [0.0, 1.0]
         * @return 0 = 成功；-1 = 失败
         */
        int (*set_duty_direct_dual)(intf_hrpwm_ch_t ch_a, float duty_a,
                                    intf_hrpwm_ch_t ch_b, float duty_b);

        /**
         * @brief 设置频率
         * @param frequency_hz 频率 [Hz]
         * @return 0 = 成功；-1 = 失败
         */
        int (*set_frequency)(uint32_t frequency_hz);

        /**
         * @brief 设置抖动比较值
         * @param ch 通道号
         * @param jitter_cmp 抖动比较值
         * @return 0 = 成功；-1 = 失败
         */
        int (*set_jitter)(intf_hrpwm_ch_t ch, uint8_t jitter_cmp);

        /**
         * @brief 启动通道输出
         * @param ch 通道号
         * @return 0 = 成功；-1 = 失败
         */
        int (*start)(intf_hrpwm_ch_t ch);

        /**
         * @brief 停止通道输出
         * @param ch 通道号
         * @return 0 = 成功；-1 = 失败
         */
        int (*stop)(intf_hrpwm_ch_t ch);

        /**
         * @brief 强制输出低电平
         * @param ch 通道号
         * @return 0 = 成功；-1 = 失败
         */
        int (*force_low)(intf_hrpwm_ch_t ch);

        /**
         * @brief 解除强制低电平
         * @param ch 通道号
         * @return 0 = 成功；-1 = 失败
         */
        int (*force_release)(intf_hrpwm_ch_t ch);

        /**
         * @brief 配置硬件故障保护
         * @param cfg 故障配置
         * @return 0 = 成功；-1 = 失败
         */
        int (*config_fault)(const intf_hrpwm_fault_cfg_t *cfg);

        /**
         * @brief 清除硬件故障
         * @return 0 = 成功；-1 = 失败
         */
        int (*clear_fault)(void);

        /**
         * @brief 配置重装载中断回调
         * @param callback 中断回调
         * @return 0 = 成功；-1 = 失败
         */
        int (*config_reload_irq)(intf_hrpwm_irq_callback_t callback);

        /**
         * @brief 使能重装载中断
         * @return 0 = 成功；-1 = 失败
         */
        int (*enable_reload_irq)(void);

        /**
         * @brief 关闭重装载中断
         * @return 0 = 成功；-1 = 失败
         */
        int (*disable_reload_irq)(void);

        /**
         * @brief 设置配对间相移
         * @param cfg 移相配置
         * @return 0 = 成功；-1 = 失败
         */
        int (*set_phase)(const intf_hrpwm_phase_cfg_t *cfg);

        /**
         * @brief 配置相移限幅
         * @param limit 相移限制配置
         * @return 0 = 成功；-1 = 失败
         */
        int (*config_phase_limit)(const intf_hrpwm_phase_limit_t *limit);

        /**
         * @brief 配置触发比较器
         * @param cmp_index 比较器索引
         * @param delay_ns 延时 [ns]
         * @return 0 = 成功；-1 = 失败
         */
        int (*config_trigger_cmp)(uint8_t cmp_index, uint32_t delay_ns);

        /**
         * @brief 设置触发比较器延时
         * @param cmp_index 比较器索引
         * @param delay_ns 延时 [ns]
         * @return 0 = 成功；-1 = 失败
         */
        int (*set_trigger_cmp_delay)(uint8_t cmp_index, uint32_t delay_ns);

        /**
         * @brief 仅启动计数器（不使能输出）
         * @return 0 = 成功；-1 = 失败
         */
        int (*start_counter_only)(void);

        /**
         * @brief 读取重装载值（诊断用，原始寄存器值）
         * @return 重装载值
         */
        uint32_t (*get_reload)(void);

        /**
         * @brief 读取比较器值（诊断用，原始寄存器值）
         * @param cmp_index 比较器索引
         * @return 比较值
         */
        uint32_t (*get_cmp_value)(uint8_t cmp_index);
    };
} intf_hrpwm_t;

/**
 * @brief 注册 HRPWM 接口实现
 * @param ops 接口实现
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_register(const intf_hrpwm_t *ops);

/**
 * @brief 初始化配对
 * @param ch 通道号
 * @param cfg 配对配置
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_init_pair(intf_hrpwm_ch_t ch, const intf_hrpwm_pair_cfg_t *cfg);

/**
 * @brief 设置占空比（经约束）
 * @param ch 通道号
 * @param duty 占空比 [0.0, 1.0]
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_set_duty(intf_hrpwm_ch_t ch, float duty);

/**
 * @brief 直接设置占空比（不经约束）
 * @param ch 通道号
 * @param duty 占空比 [0.0, 1.0]
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_set_duty_direct(intf_hrpwm_ch_t ch, float duty);

/**
 * @brief 同时直接设置两通道占空比
 * @param ch_a 通道 A
 * @param duty_a 通道 A 占空比 [0.0, 1.0]
 * @param ch_b 通道 B
 * @param duty_b 通道 B 占空比 [0.0, 1.0]
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_set_duty_direct_dual(intf_hrpwm_ch_t ch_a, float duty_a,
                                    intf_hrpwm_ch_t ch_b, float duty_b);

/**
 * @brief 设置实例频率
 * @param inst 实例号
 * @param frequency_hz 频率 [Hz]
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_set_frequency(intf_hrpwm_inst_t inst, uint32_t frequency_hz);

/**
 * @brief 设置抖动比较值
 * @param ch 通道号
 * @param jitter_cmp 抖动比较值
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_set_jitter(intf_hrpwm_ch_t ch, uint8_t jitter_cmp);

/**
 * @brief 启动通道输出
 * @param ch 通道号
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_start(intf_hrpwm_ch_t ch);

/**
 * @brief 停止通道输出
 * @param ch 通道号
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_stop(intf_hrpwm_ch_t ch);

/*
 * 仅启动 PWM 计数器 (CEN)，不使能物理输出引脚。
 * 用于软启动前预热 ADC PMT 触发链 (CMP10/11 依赖计数器运行)，
 * 引脚仍保持关闭，避免占空比未定前的输出冲击。
 * 后续需调用 intf_hrpwm_start() 使能实际输出。
 */

/**
 * @brief 仅启动实例计数器（不使能输出引脚）
 * @param inst 实例号
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_start_counter_only(intf_hrpwm_inst_t inst);

/**
 * @brief 读取实例重装载值（诊断用，原始寄存器值）
 * @param inst 实例号
 * @return 重装载值；实例非法或未注册返回 0
 */
uint32_t intf_hrpwm_get_reload(intf_hrpwm_inst_t inst);

/**
 * @brief 读取实例比较器值（诊断用，原始寄存器值）
 * @param inst 实例号
 * @param cmp_index 比较器索引
 * @return 比较值；实例非法或未注册返回 0
 */
uint32_t intf_hrpwm_get_cmp_value(intf_hrpwm_inst_t inst, uint8_t cmp_index);

/**
 * @brief 强制输出低电平
 * @param ch 通道号
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_force_low(intf_hrpwm_ch_t ch);

/**
 * @brief 解除强制低电平
 * @param ch 通道号
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_force_release(intf_hrpwm_ch_t ch);

/**
 * @brief 配置硬件故障保护
 * @param inst 实例号
 * @param cfg 故障配置
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_config_fault(intf_hrpwm_inst_t inst, const intf_hrpwm_fault_cfg_t *cfg);

/**
 * @brief 清除硬件故障
 * @param inst 实例号
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_clear_fault(intf_hrpwm_inst_t inst);

/* 中断配置接口 */

/**
 * @brief 配置重装载中断回调
 * @param inst 实例号
 * @param callback 中断回调
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_config_reload_irq(intf_hrpwm_inst_t inst, intf_hrpwm_irq_callback_t callback);

/**
 * @brief 使能重装载中断
 * @param inst 实例号
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_enable_reload_irq(intf_hrpwm_inst_t inst);

/**
 * @brief 关闭重装载中断
 * @param inst 实例号
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_disable_reload_irq(intf_hrpwm_inst_t inst);

/* 移相配置接口 */

/**
 * @brief 设置配对间相移
 * @param cfg 移相配置
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_set_phase(const intf_hrpwm_phase_cfg_t *cfg);

/**
 * @brief 配置相移限幅
 * @param inst 实例号
 * @param limit 相移限制配置
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_config_phase_limit(intf_hrpwm_inst_t inst, const intf_hrpwm_phase_limit_t *limit);

/*
 * PWM 触发信号配置（用于 ADC 同步等）
 *   delay_ns：计数谷底（回卷点）之后的延时，单位 ns。
 *   驱动按 PWM 时钟换算为 tick（tick = clk × delay_ns / 1e9），与 PWM 频率解耦。
 *   比较器匹配后经 CHxREF → TRGM 输出触发脉冲（每周期一次）。
 */

/**
 * @brief 配置触发比较器
 * @param inst 实例号
 * @param cmp_index 比较器索引
 * @param delay_ns 延时 [ns]
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_config_trigger_cmp(intf_hrpwm_inst_t inst, uint8_t cmp_index, uint32_t delay_ns);

/**
 * @brief 设置触发比较器延时
 * @param inst 实例号
 * @param cmp_index 比较器索引
 * @param delay_ns 延时 [ns]
 * @return 0 = 成功；-1 = 失败
 */
int intf_hrpwm_set_trigger_cmp_delay(intf_hrpwm_inst_t inst, uint8_t cmp_index, uint32_t delay_ns);

#ifdef __cplusplus
}
#endif

#endif /* INTF_HRPWM_H */
