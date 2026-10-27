/**
 * @file    app_fault.h
 * @brief   故障保护与错误处理（v1：纯判断）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_FAULT_H
#define APP_FAULT_H

#include "app_adc.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 故障码（32 位位图；bit10-31 预留：温度等）
 * ============================================================================ */
#define APP_FAULT_NONE        (0x00000000UL)
#define APP_FAULT_OC_FAST_U   (1UL << 0) /* L1：ADC WDOG 硬件阈值（µs 级） */
#define APP_FAULT_OC_FAST_V   (1UL << 1)
#define APP_FAULT_OC_FAST_W   (1UL << 2)
#define APP_FAULT_OC_SLOW_U   (1UL << 3) /* L2：RMS 连续越限（1kHz 去抖） */
#define APP_FAULT_OC_SLOW_V   (1UL << 4)
#define APP_FAULT_OC_SLOW_W   (1UL << 5)
#define APP_FAULT_VBUS_OV     (1UL << 6) /* L3：母线过压 */
#define APP_FAULT_VBUS_UV     (1UL << 7) /* L3：母线欠压 */
#define APP_FAULT_ADC_TIMEOUT (1UL << 8) /* 健康：PMT 帧序号停滞 */
#define APP_FAULT_ENC_READ    (1UL << 9)  /* 健康：编码器错误计数增量 */
#define APP_FAULT_OC_TRIP     (1UL << 10) /* 快跳：电流环内置 i_trip_a 连续超限 */

/* 故障码位数（bit0..10）：policy 表与"带动作码掩码"的规模 */
#define APP_FAULT_CODE_COUNT  (11U)

/* DERATE 生效时的 iq 限幅倍率（待接入 YAML；见计划 Task 7） */
#define APP_FAULT_DERATE_SCALE_DEFAULT (0.5f)

/* L3 默认值：一级沿用 v1 的 5 拍去抖（保持行为一致）；二级 40V（24V 系统的 1.67×） */
#define APP_FAULT_VBUS_OV_TRIP_V_DEFAULT   (40.0f)
#define APP_FAULT_VBUS_DEBOUNCE_MS_DEFAULT (5U)

/* ============================================================================
 * 状态机
 * ============================================================================ */
/**
 * @brief 故障状态机状态
 */
typedef enum {
    APP_FAULT_STATE_INIT = 0, /**< 初始化 */
    APP_FAULT_STATE_NORMAL,   /**< 正常 */
    APP_FAULT_STATE_WARNING,  /**< 去抖计数中（条件已出现，未达次数） */
    APP_FAULT_STATE_FAULT,    /**< 已触发（锁存；v1 无动作） */
} app_fault_state_t;

/* ============================================================================
 * 阈值默认值来源：config/software.yaml（app_software_params.fault，app_fault_init 加载）
 * 以下为内部时序量（待 FOC 时序体系定义后接入 YAML，见参数管线设计 §10）
 * ============================================================================ */
#define APP_FAULT_RMS_WINDOW_DEFAULT \
    (250U) /* 10ms @25kHz（TBD：FOC 时序体系接入后随 pwm_freq 派生） */
#define APP_FAULT_SETTLE_TICKS_DEFAULT (50U) /* 上电静默期（≈50ms @1kHz） */

/* L1 WDOG 自动重臂（1kHz tick 计数；1ms = 1 tick）。消除 v1 spec §10 记录的
 * "L1 WDOG 为一次性：命中后驱动自动关该通道中断，仅 F 清除时重装"缺口。 */
#define APP_FAULT_WDOG_REARM_MS_DEFAULT  (100U) /* 命中后允许重臂的等待时间 [ms] */
#define APP_FAULT_WDOG_REARM_MAX_DEFAULT (5U)   /* 单次上电重臂上限；超限不再自动重臂 */

/**
 * @brief 故障阈值配置（0 = 使用 app_software_params.fault 默认值）
 */
typedef struct {
    float oc_fast_a;        /**< L1 相电流阈值 [A]，0 = 默认 72.9A */
    float oc_slow_a;        /**< L2 RMS 阈值 [A]，0 = 默认 72.9A */
    float vbus_ov_v;        /**< 过压 [V]，0 = 默认 36V */
    float vbus_uv_v;        /**< 欠压 [V]，0 = 默认 9V */
    uint16_t slow_debounce; /**< L2/L3 去抖次数，0 = 默认 5 */
    uint16_t adc_stall_ms;  /**< PMT 帧超时 [ms]，0 = 默认 10 */
    uint8_t enc_err_delta;  /**< 编码器错误增量阈值，0 = 默认 3 */
} app_fault_cfg_t;

/**
 * @brief 首故障快照（进入 FAULT 时记录一次）
 */
typedef struct {
    uint32_t code;        /**< 首故障码（单个位） */
    float i_u_a;          /**< U 相电流 [A] */
    float i_v_a;          /**< V 相电流 [A] */
    float i_w_a;          /**< W 相电流 [A] */
    float v_bus_v;        /**< 母线电压 [V] */
    uint16_t oc_fast_raw; /**< L1 触发时的原始码（0 = 非 L1） */
} app_fault_snapshot_t;

/* ============================================================================
 * API
 * ============================================================================ */

/**
 * @brief 初始化（须在 app_adc_init 之前调用：提供 WDOG 阈值与回调）
 * @param cfg 故障阈值配置；NULL = 全默认
 */
void app_fault_init(const app_fault_cfg_t* cfg);

/**
 * @brief 25kHz 主循环：三相电流 RMS 累加
 */
void app_fault_process(void);

/**
 * @brief 1kHz（ADC1 序列完成回调，ISR 上下文；驱动保证每帧一次）：
 *        L2/L3/健康判断 + 执行清除请求
 */
void app_fault_tick(void);

/**
 * @brief L1：ADC0 WDOG 回调（ISR 上下文）
 * @param ch 逻辑通道
 * @param value WDOG 触发时的原始码
 * @param user 用户上下文（未使用）
 */
void app_fault_on_wdog(adc_channel_t ch, uint16_t value, void* user);

/**
 * @brief 清除锁存（条件须已恢复；完整复位——状态/锁存/首故障/快照/事件计数）。
 *        内部使用全局中断临界区，与故障 ISR 互斥。
 * @return 0 = 已清除；-1 = 条件未恢复/未初始化
 */
int app_fault_clear(void);

/**
 * @brief 当前故障状态机状态
 * @return 当前状态
 */
app_fault_state_t app_fault_get_state(void);

/**
 * @brief 当前存在的条件位图（条件恢复后自动解除；ENC_READ 为事件型仅经清除解除）
 * @return 当前条件位图
 */
uint32_t app_fault_get_codes(void);

/**
 * @brief 锁存故障位图（条件消失亦保持，直至清除）
 * @return 锁存故障位图
 */
uint32_t app_fault_get_latched(void);

/**
 * @brief 首故障码
 * @return 首故障码（单个位；0 = 无）
 */
uint32_t app_fault_get_first(void);

/**
 * @brief 各故障事件计数（按 bit0..；自上次清除以来）
 * @param out 输出数组
 * @param n 输出数组长度
 */
void app_fault_get_event_counts(uint16_t* out, uint8_t n);

/**
 * @brief 读取首故障快照
 * @param out 输出快照
 * @return true = 快照有效
 */
bool app_fault_get_snapshot(app_fault_snapshot_t* out);

/**
 * @brief 逐故障动作级别（2026-10-27 v2：取代"全开/全关"单一布尔）
 */
typedef enum {
    APP_FLT_ACT_WARN = 0,   /**< 只计数/告警（= v1 全部行为） */
    APP_FLT_ACT_DERATE = 1, /**< 压低 iq 上限（`app_fault_get_limit_scale`）；条件恢复自动解除 */
    APP_FLT_ACT_STOP = 2,   /**< 受控停机（发起 + deadline，由慢保护推进） */
    APP_FLT_ACT_TRIP = 3,   /**< 立即关桥（force_low + 关 +12V + 锁存抑制） */
} app_fault_action_t;

/**
 * @brief 设计动作表（不随台架总保险变化，用于评审与回归锁定设计）
 * @param code 单个故障码位
 * @return 设计动作级别；非故障码返回 WARN
 */
app_fault_action_t app_fault_policy_default(uint32_t code);

/** @brief 生效动作表（`fault.actions_enable=0` 时全部退为 WARN） */
app_fault_action_t app_fault_get_policy(uint32_t code);
/** @brief 运行期改生效表（RAM，不持久化） */
void app_fault_set_policy(uint32_t code, app_fault_action_t act);
/** @brief 当前生效的最高级动作（TRIP > STOP > DERATE > WARN） */
app_fault_action_t app_fault_get_action(void);
/** @brief DERATE 生效时的 iq 限幅倍率 [0,1]；无 DERATE = 1.0 */
float app_fault_get_limit_scale(void);
/** @brief 锁存集中"带动作码"（STOP/TRIP）的位图 —— FOC 使能拒绝条件 */
uint32_t app_fault_get_blocking_latched(void);
/**
 * @brief 台架注入（RAM）：不经物理阈值地置/清某个故障码
 *
 * 用途：逐条验证动作路径（WARN/DERATE/STOP/TRIP），比"改阈值法"更安全。
 * 注入位置独立于真实检测（不污染 active/latched 的物理语义）。
 * @return 0 = 成功；-1 = 非故障码
 */
int app_fault_inject(uint32_t code, bool on);
/**
 * @brief 由其他模块登记故障码（ISR 安全；如 FOC 侧的快跳）
 * @param code 单个故障码位
 */
void app_fault_raise(uint32_t code);
/**
 * @brief 设置 L3 母线两级阈值与 ms 级去抖（0 = 保持当前值）
 *
 * 一级 `ov_v` + `ov_ms` 拍 → 按 policy（设计表为 STOP）；
 * 二级 `ov_trip_v` 单拍越限 → 直接 TRIP（无去抖，硬件风险优先）；
 * 欠压 `uv_v` + `uv_ms` 拍 → 按 policy（设计表为 STOP）。
 * 注意：二级仅在 `policy(VBUS_OV) ∈ {STOP,TRIP}` 时生效 —— 台架全 WARN 时不改变行为。
 */
void app_fault_set_vbus_levels(float ov_v, float ov_trip_v, float uv_v, uint16_t ov_ms,
                               uint16_t uv_ms);

/**
 * @brief 取一次"待发起的受控停机"请求（边沿触发，读取即清除）
 * @return 故障码（单个位）；0 = 无请求。由 app_foc_run_once 消费。
 */
uint32_t app_fault_take_stop_request(void);

/**
 * @brief 调用方报告"受控停机不可用"（OFF-但桥使能 / CALIB / vtest）→ 动作升为 TRIP
 */
void app_fault_note_stop_rejected(void);

/**
 * @brief 设置 DERATE 生效时的 iq 限幅倍率（0 = 保持当前值）
 * @param scale (0,1]；由 app_foc_current_get_i_limit() 乘入 → 自动传播到 FOC 与 motion
 */
void app_fault_set_derate_scale(float scale);

/**
 * @brief 保护动作总开关（运行期；源自 software.fault.actions_enable，ILM 常驻）
 *
 * `true`  = 按 policy 表执行动作（STOP/TRIP/DERATE）；
 * `false` = 仅警告，不改写任何控制输出（台架默认；行为与 v1 逐字段一致）。
 * 取代原编译期宏（`app_protect_policy.h` 已于 2026-10-27 删除）。
 */
bool app_fault_actions_enabled(void);

/** @brief 运行期覆盖总开关（RAM；等价 software.fault.actions_enable 的运行期版本） */
void app_fault_set_actions_enable(bool on);

/** @brief skill: 台架注入集合（诊断） */
uint32_t app_fault_get_injected(void);

/**
 * @brief L1 WDOG 自动重臂服务（**1kHz 任务上下文**，由 app_io_step 调用）
 *
 * 命中后经 `rearm_ms` 且该相 raw 已回到窗内 → `app_adc_wdog_reenable()`；
 * 单次上电重臂次数达 `rearm_max` → 永久停止自动重臂（升 TRIP 锁存，仅清除复位）。
 * 依据：驱动在 WDOG 命中后会自动关闭该通道中断（避免刷屏），若不重臂则该相
 * 硬件保护在此后长期失效。
 */
void app_fault_wdog_service(void);

/**
 * @brief 设置 L1 WDOG 自动重臂参数（0 = 用默认值）
 * @param rearm_ms 命中后等待 [ms]（1kHz tick）
 * @param rearm_max 单次上电重臂上限
 */
void app_fault_set_wdog_rearm(uint16_t rearm_ms, uint16_t rearm_max);

/**
 * @brief 某相自上次清除以来已自动重臂次数
 * @param phase 0 = U，1 = V，2 = W
 * @return 重臂次数；参数非法返回 0
 */
uint16_t app_fault_get_wdog_rearm_count(uint8_t phase);

/**
 * @brief 取 WDOG 原始阈值窗口（供 app_adc 配置；init 后有效）
 * @param thshd_high 高阈值输出
 * @param thshd_low 低阈值输出
 */
void app_fault_get_wdog_raw(uint16_t* thshd_high, uint16_t* thshd_low);

#ifdef __cplusplus
}
#endif

#endif /* APP_FAULT_H */
