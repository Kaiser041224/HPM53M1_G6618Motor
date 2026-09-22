/**
 * @file    app_debug_foc.h
 * @brief   FOC 调试/观测结构（Ozone 直读 + 一次性命令）
 * @author  Kaiser
 *
 * 设计（见 docs/superpowers/specs/2026-09-22-encoder-sampler-foc-realtime-design.md §2.9）：
 *   - `g_app_debug_foc` 置于 .noncacheable.bss，调试器（Ozone）直接读/写
 *   - 请求域与状态域分离：命令一次性（ack/result），状态每拍刷新
 *   - 无 Control→Debug 依赖：本层依赖 Control/Interface，反向不成立
 *   - 命令由主循环 `app_debug_foc_tick()` 有界处理（每拍最多 N 条）
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_DEBUG_FOC_H
#define APP_DEBUG_FOC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 一次性命令（写入 g_app_debug_foc.command）
 */
typedef enum {
    APP_DEBUG_FOC_CMD_NONE = 0,   /**< 空闲 */
    APP_DEBUG_FOC_CMD_ENABLE,     /**< 使能 FOC（先清零目标给定） */
    APP_DEBUG_FOC_CMD_DISABLE,    /**< 关闭 FOC（先清零目标给定） */
    APP_DEBUG_FOC_CMD_CLEAR_FAULT,/**< 清除故障锁存（app_fault_clear） */
    APP_DEBUG_FOC_CMD_VTEST,      /**< 开环电压诊断（arg0 = 电压 x1000，arg1 = θe x10000 [rad]） */
    APP_DEBUG_FOC_CMD_CAL_ENCODER,/**< 启动电气标定（app_motor_identify_start） */
    APP_DEBUG_FOC_CMD_CAL_ABORT,  /**< 中止电气标定（app_motor_identify_abort） */
    APP_DEBUG_FOC_CMD_PASSIVE_SELFTEST, /**< 被动自检（不使能桥；app_debug_encoder_passive_selftest） */
} app_debug_foc_cmd_t;

/**
 * @brief FOC 调试结构（调试器直读；volatile 非缓存）
 *
 * 请求域（调试器写；主循环 tick 消费并回 ack）：
 *   command / arg0 / arg1（一次性命令）
 * 状态域（固件写；调试器只读）：
 *   state..enc_jumps、i_d..tripped 等
 */
typedef struct {
    /* ---- 请求域 ---- */
    volatile uint32_t command;       /**< app_debug_foc_cmd_t 当前待处理命令 */
    volatile int32_t  arg0;          /**< 命令参数 0 */
    volatile int32_t  arg1;          /**< 命令参数 1 */
    volatile uint32_t ack_sequence;  /**< 已处理命令计数（完成即 +1） */
    volatile int32_t  ack_result;    /**< 最近命令返回值（0 = 成功） */
    /* 运行期给定（调试器可写；主循环仅在 READY/RUN 消费；enable/disable 清零） */
    volatile float    iq_target_a;   /**< q 轴目标给定 [A]（经 app_foc_set_iq_ref 限幅） */
    volatile float    id_target_a;   /**< d 轴目标给定 [A] */
    /* 紧急停机请求（写 1 → ADC ISR 直接消费，无需主循环；disable 清零） */
    volatile uint32_t estop_request; /**< 非 0 = 请求紧急停机（Control 持有其指针） */

    /* ---- 状态域 ---- */
    volatile uint32_t ready;         /**< FOC 初始化就绪 */
    volatile uint32_t enabled;       /**< FOC 活动（state != OFF） */
    volatile uint32_t state;         /**< app_foc_state_t */
    volatile uint32_t fault_codes;   /**< 当前故障条件位图 */
    volatile uint32_t fault_latched; /**< 锁存故障位图 */
    volatile uint32_t tripped;       /**< 快速过流跳闸锁存 */
    volatile uint32_t saturated;     /**< 本拍 PI 圆形限幅触发 */

    volatile uint32_t isr_cycles;     /**< 25kHz ISR 本拍耗时 [cycle] */
    volatile uint32_t isr_cycles_max; /**< 历史最大 [cycle] */
    volatile uint32_t isr_overruns;   /**< 超预算次数 */
    volatile uint32_t isr_run_count;  /**< ISR 健康拍计数（快照 run_count） */

    volatile uint32_t enc_seq;       /**< 最近接受的编码器样本序号 */
    volatile uint32_t enc_age_us;    /**< 快照年龄 [µs] */
    volatile uint32_t enc_age_cycles;/**< 快照年龄 [cycle] */
    volatile uint32_t enc_errors;    /**< 传输错误累计 */
    volatile uint32_t enc_jumps;     /**< 跳变（坏帧）累计 */
    volatile uint32_t enc_read_fail; /**< 采样器读失败累计（g_encoder_read_fail_count） */
    volatile uint32_t enc_isr_cycles;/**< GPTMR 采样 ISR 单拍耗时 [cycle] */
    volatile uint32_t enc_isr_cycles_max; /**< GPTMR 采样 ISR 历史最大 [cycle] */

    volatile uint32_t estop_ack;     /**< = g_foc_fault_request（ISR 紧急停机请求回读） */
    volatile uint32_t isr_inhibited; /**< ISR 输出抑制锁存（故障后为 1，disable 清除） */
    volatile float    duty_max_eff;  /**< 控制层实际生效占空比上限（= min(user, 保守 0.70)） */

    /* 运行期健康自检（不触碰 SPI3；PASSIVE_SELFTEST 命令启动，100ms 出结果） */
    volatile uint32_t selftest_active;      /**< 1 = 进行中 */
    volatile uint32_t selftest_ok;          /**< 1 = 上次窗口通过 */
    volatile uint32_t selftest_progress;    /**< 0..100 */
    volatile uint32_t selftest_seq_delta;   /**< 窗口内接受样本增量 */
    volatile uint32_t selftest_valid_pct;   /**< 有效快照占比 0..100 */
    volatile uint32_t selftest_read_fail_delta; /**< 读失败增量 */
    volatile uint32_t selftest_jump_delta;      /**< 跳变增量 */
    volatile uint32_t selftest_age_max_us;      /**< 窗口内最大快照年龄 [µs] */

    /* 电气标定（app_motor_identify）状态/结果 */
    volatile uint32_t cal_active;     /**< 辨识进行中 */
    volatile uint32_t cal_done;       /**< 完成且验证通过 */
    volatile uint32_t cal_failed;     /**< 失败 */
    volatile uint32_t cal_fail_reason;/**< app_identify_fail_t */
    volatile float    cal_progress;   /**< 0~1 */
    volatile float    cal_offset_rad; /**< 辨识电角度零点 [rad] */
    volatile float    cal_direction;  /**< 辨识方向 (+1/-1) */
    volatile float    cal_quality;    /**< 辨识质量 */

    volatile float    i_d_a;         /**< d 轴电流 [A] */
    volatile float    i_q_a;         /**< q 轴电流 [A] */
    volatile float    i_d_avg_a;     /**< d 轴慢平均 [A] */
    volatile float    i_q_avg_a;     /**< q 轴慢平均 [A] */
    volatile float    i_d_ref_a;     /**< d 轴给定（限幅后）[A] */
    volatile float    i_q_ref_a;     /**< q 轴给定（限幅后）[A] */
    volatile float    duty_u;        /**< U 相占空比 */
    volatile float    duty_v;        /**< V 相占空比 */
    volatile float    duty_w;        /**< W 相占空比 */
    volatile float    v_bus_v;       /**< 母线电压 [V] */
    volatile float    v_scale;       /**< 调制缩放（<1 = 饱和） */
    volatile float    theta_e_rad;   /**< 本拍电角度 [rad] */
    volatile float    omega_e_rad_s; /**< 本拍电角速度 [rad/s] */

    volatile uint32_t tick_count;    /**< 主循环 tick 计数（活性指示） */
} app_debug_foc_t;

/** Ozone 观测/命令结构（.noncacheable.bss；调试器直读直写） */
extern volatile app_debug_foc_t g_app_debug_foc;

/**
 * @brief 初始化调试结构（清零 + 置就绪）
 */
void app_debug_foc_init(void);

/**
 * @brief 主循环每拍调用：刷新状态域 + 有界处理一次性命令（每拍最多 2 条）
 */
void app_debug_foc_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_DEBUG_FOC_H */
