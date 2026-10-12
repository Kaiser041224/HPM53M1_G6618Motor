/**
 * @file    app_foc.h
 * @brief   FOC 编排：状态机 / 模式 / 联锁 / 25kHz 入口
 * @author  Kaiser
 *
 * 状态机（spec §4.1）：
 *   OFF → (enable) → READY → (首次非零给定) → RUN
 *   READY/RUN → (cal 请求) → CALIB → (完成/中止) → READY
 *   任意 → (fault != NORMAL) → FAULT（零矢量 + 关桥）→ (disable) → OFF
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_FOC_H
#define APP_FOC_H

#include <stdbool.h>
#include <stdint.h>

#include "app_foc_current.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FOC 状态
 */
typedef enum {
    APP_FOC_STATE_OFF = 0, /**< 输出关闭 */
    APP_FOC_STATE_READY,   /**< 桥已使能，零矢量 */
    APP_FOC_STATE_RUN,     /**< 电流闭环 */
    APP_FOC_STATE_CALIB,   /**< 辨识模式（强制角 + 辨识模块驱动） */
    APP_FOC_STATE_FAULT,   /**< 故障门控（零矢量 + 关桥） */
} app_foc_state_t;

/**
 * @brief 角度来源
 */
typedef enum {
    APP_FOC_ANGLE_ENCODER = 0, /**< 编码器（正常闭环） */
    APP_FOC_ANGLE_FORCED,      /**< 强制角（辨识/调试） */
    APP_FOC_ANGLE_OBSERVER,    /**< 观测器（无感占位；V1 未实现） */
} app_foc_angle_source_t;

/**
 * Ozone 观测：FOC 单拍耗时 [cycle]（.noncacheable.bss；25kHz 节拍）
 */
extern volatile uint32_t g_foc_loop_cycles;
/** Ozone 观测：FOC 调用间隔 [µs]（25kHz 标称；明显大于 40 = 节拍抖动/丢拍） */
extern volatile uint32_t g_foc_loop_dt_us;
/** Ozone 观测：25kHz ISR 单拍耗时 [cycle] / 历史最大 */
extern volatile uint32_t g_foc_isr_cycles;
extern volatile uint32_t g_foc_isr_cycles_max;
/** Ozone 观测：ISR 单拍超预算次数（超限即停用快路径并紧急关桥） */
extern volatile uint32_t g_foc_isr_overruns;
/** Ozone 观测：编码器快照年龄与序号（ISR 侧） */
extern volatile uint32_t g_foc_enc_age_cycles;
extern volatile uint32_t g_foc_enc_age_us;
extern volatile uint32_t g_foc_enc_seq;
/** ISR→主循环 故障请求（ISR 只置位；主循环负责状态迁移） */
extern volatile uint32_t g_foc_fault_request;

/**
 * @brief 初始化（上电状态 OFF）
 */
void app_foc_init(void);

/**
 * @brief 绑定外部紧急停机请求邮箱（Control 持有指针；ISR 每拍只读，无需主循环）。
 * @param req 请求字指针（*req != 0 → ISR 立即紧急关桥并锁存抑制）；可为 NULL 解绑。
 * @note 典型绑定：Debug 层把一个 volatile 静态字段地址传入；Ozone 直接写该字段=1
 *       即可在无主循环参与下触发停机。生命周期须覆盖运行期。
 *       触发后 *req 会被回写为 1（锁存），disable() 会清零。
 */
void app_foc_register_estop_request(volatile uint32_t* req);

/**
 * @brief ISR 是否已锁存输出抑制（故障后为 true；disable 清除）
 */
bool app_foc_isr_inhibited(void);

/**
 * @brief 25kHz 节拍（app_logic 主循环调用）
 */
void app_foc_run_once(void);

/**
 * @brief FOC 快速路径（ADC0 PMT 完成中断内调用；由 app_foc_init 自动注册）
 * @note 编码器采样 + 角度链 + 电流环；固定 25kHz。禁止在中断外直接调用。
 */
void app_foc_isr_step(void);

/**
 * @brief 使能（OFF → READY）：检查故障/ADC/编码器/参数 → 使能逆变桥
 * @return 0 = 成功；-1 = 拒绝
 */
int app_foc_enable(void);

/**
 * @brief 关闭（任意 → OFF）：零矢量 + 关桥
 */
void app_foc_disable(void);
/** @brief 用户关闭：100个1ms任务拍缓降+20拍零给定，非阻塞 */
int app_foc_request_stop(void);
/** @brief 正常关闭尚未完成，期间拒绝新给定/校准 */
bool app_foc_stop_pending(void);
/** @brief OFF时设置占空比上限[0.70,0.98]，RAM；重启默认0.95；高值采样精度未验证 */
int app_foc_set_duty_ceiling(float ceiling);

/**
 * @brief 设置转矩给定（限幅 ±i_q_max）
 * @param i_q_a q 轴电流给定 [A]
 * @return 0 = 成功；-1 = 状态不允许/参数非法
 */
int app_foc_set_iq_ref(float i_q_a);

/**
 * @brief 读取当前 q 轴给定（已限幅；命令回显用）
 * @return q 轴给定 [A]
 */
float app_foc_get_iq_ref(void);

/**
 * @brief 设置 d 轴给定（辨识/调试；默认 0）
 * @param i_d_a d 轴给定 [A]（不做单独限幅：内部电流矢量限幅兜底）
 * @return 0 = 成功；-1 = 状态不允许/参数非法
 */
int app_foc_set_id_ref(float i_d_a);

/**
 * @brief 设置角度来源与强制角
 * @param src 角度来源（编码器 / 强制）
 * @param theta_e_rad 强制电角度 [rad]（src = ENCODER 时忽略）
 * @return 0 = 成功；-1 = 枚举非法/非有限值/状态不允许
 */
int app_foc_set_angle_source(app_foc_angle_source_t src, float theta_e_rad);

/**
 * @brief 当前状态
 * @return 状态枚举
 */
app_foc_state_t app_foc_get_state(void);

/**
 * @brief 是否活动（!= OFF；供 Debug/Comm 互斥判断）
 * @return true = 活动
 */
bool app_foc_is_active(void);

/**
 * @brief 读取电流环快照
 * @param out 输出快照（不可为 NULL）
 */
void app_foc_get_snapshot(app_foc_current_snapshot_t* out);

/* ============================================================================
 * 校准端口（Calibration Port）
 *
 * 供隔离的校准模块（app_motor_identify）在 1kHz 任务上下文借用电流环：
 *   begin → set_excitation（强制角激励）/ use_trial_offset + set_current（试用零点验证）→ end
 * 约束：
 *   - 仅 CALIB 状态生效；FOC 不感知校准算法，ISR 无新增代码（复用 CALIB 角源/给定）；
 *   - 运行零点由镜像持续记录（仅 apply / enable 更新），end 时据此恢复（试用零点永不残留）；
 *   - 校准结果生效只经 app_foc_apply_encoder_offset（由调用方显式发起）。
 * ============================================================================ */

/**
 * @brief 开始校准（READY/RUN → CALIB；强制角 0、给定清零、PI 积分清零）
 * @return 0 = 成功；-1 = 状态不允许（非 READY/RUN 或 vtest 进行中）
 */
int app_foc_calib_begin(void);
/** @brief CALIB专用同步捕获，可选有限d轴电压脉冲 */
int app_foc_calib_capture(float voltage, bool voltage_mode);
/** @brief CALIB专用临时PI，不修改正式软件参数 */
int app_foc_calib_trial_pi(float kp, float ki);
/** @brief 有限恒压租约；voltage<0退出恒压，周期数由ADC拍数计数 */
int app_foc_calib_voltage(float voltage, uint32_t cycles);
/** @brief 恒压偏置周围同步交替激励，half_period单位ADC拍 */
int app_foc_calib_pulse(float amplitude, uint16_t half_period);

/**
 * @brief 结束校准（CALIB → READY；恢复运行零点与编码器角源、给定清零）；非 CALIB 时无操作
 */
void app_foc_calib_end(void);

/**
 * @brief 强制角激励（CALIB；切换为强制角源）
 * @param theta_e_rad 强制电角度 [rad]
 * @param i_d_ref d 轴电流给定 [A]
 * @param i_q_ref q 轴电流给定 [A]
 * @return 0 = 成功；-1 = 非 CALIB / 参数非有限
 */
int app_foc_calib_set_excitation(float theta_e_rad, float i_d_ref, float i_q_ref);

/**
 * @brief 试用零点（CALIB；切换为编码器角源并临时使用给定零点/方向，end 时作废）
 * @param offset_rad 试用电角度零点 [rad]
 * @param direction 试用方向（+1.0 / −1.0）
 * @return 0 = 成功；-1 = 非 CALIB / 参数非法
 */
int app_foc_calib_use_trial_offset(float offset_rad, float direction);

/**
 * @brief 设置 d/q 电流给定（CALIB；不改变角源）
 * @param i_d_ref d 轴电流给定 [A]
 * @param i_q_ref q 轴电流给定 [A]
 * @return 0 = 成功；-1 = 非 CALIB / 参数非有限
 */
int app_foc_calib_set_current(float i_d_ref, float i_q_ref);

/**
 * @brief 应用电角度零点/方向到运行期角度链（不写参数；CALIB 中拒绝）
 * @param offset_rad 电角度零点 [rad]
 * @param direction 方向（+1.0 / −1.0）
 * @return 0 = 成功；-1 = CALIB 中 / 参数非法
 * @note app_foc_enable 会从电机参数重建角度链：持久生效须同时写电机参数
 *       （app_motor_identify_apply 已一并处理）。
 */
int app_foc_apply_encoder_offset(float offset_rad, float direction);

/**
 * @brief 读取运行期角度链当前生效的零点/方向（不含 CALIB 试用值）
 * @param offset_rad 输出零点 [rad]（可为 NULL）
 * @param direction 输出方向（可为 NULL）
 */
void app_foc_get_encoder_offset(float* offset_rad, float* direction);

/**
 * @brief 是否处于 FAULT 门控
 * @return true = 当前状态为 FAULT
 */
bool app_foc_fault_gate(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_FOC_H */
