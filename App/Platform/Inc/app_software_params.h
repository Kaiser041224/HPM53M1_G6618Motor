/**
 * @file    app_software_params.h
 * @brief   软件参数（工厂默认，来源 config/software.yaml）
 * @author  Kaiser
 *
 * 消费者：app_fault（阈值）/ app_can（波特率）/ app_debug_can（回报帧 ID）/ FOC（后续，PID
 * 与限幅）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_SW_PARAMS_H
#define APP_SW_PARAMS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief CAN 通信参数
 */
typedef struct {
    uint32_t baudrate;   /**< CAN 波特率 [bps] */
    uint8_t
        node_id_default; /**< 默认节点号（占位：DIP 解码未实现；与 CAN ID 的派生关系待协议定稿） */
    uint32_t rx_control_id; /**< 接收控制帧 CAN ID（占位：待协议定稿） */
    uint32_t tx_report_id;  /**< 参数回报帧 CAN ID（占位：待协议定稿） */
} app_software_can_t;

/**
 * @brief 故障保护阈值
 */
typedef struct {
    uint8_t actions_enable; /**< 1 = 启用保护动作（按逐故障 policy 表）；0 = 仅警告（不改写任何输出，台架默认） */
    float oc_trip_a;     /**< 过流阈值 [A] */
    float vbus_ov_v;     /**< 母线过压 [V] */
    float vbus_uv_v;     /**< 母线欠压 [V] */
    uint16_t slow_debounce; /**< L2/L3 去抖次数（1kHz） */
    uint16_t adc_stall_ms;  /**< PMT 帧停滞超时 [ms] */
    uint8_t enc_err_delta;  /**< 编码器错误增量阈值 */
} app_software_fault_t;

/**
 * @brief 速度环参数（app_motion init 期消费；motion gains 在线改并可 param save 持久化）
 */
typedef struct {
    float kp;                            /**< 比例增益 [Nm/(rad/s)]（轴端；0 = 禁用速度模式） */
    float ki;                            /**< 积分增益 [Nm/rad]（轴端） */
    float friction_coulomb_nm;           /**< 库仑摩擦前馈 Tc [Nm]（平滑符号叠加） */
    float friction_viscous_nm_per_rad_s; /**< 粘性摩擦前馈 Kv [Nm/(rad/s)] */
    float friction_smoothing_rad_s;      /**< 平滑符号速度尺度 [rad/s]（0=硬符号） */
    float velocity_lpf_coeff;            /**< 测速一阶滤波系数（有效域 (0,1]） */
} app_software_speed_loop_t;

/**
 * @brief 位置环参数（app_motion init 期消费；motion pos_gains 在线改并可 param save 持久化）
 */
typedef struct {
    float pos_gain; /**< 位置环P [1/s]（vel_cmd += pos_gain*pos_err；0 = 禁用位置模式） */
    float input_filter_bandwidth; /**< 输入二阶滤波带宽 [rad/s]（0 = 直通） */
} app_software_position_loop_t;

/**
 * @brief 控制限幅参数（FOC 电流环消费）
 */
typedef struct {
    float i_q_max_a;                /**< 电流 boost/绝对上限 [A_peak]（连续值见 i_q_continuous_a） */
    float duty_max;                 /**< 调制上限（有效域 (0.5, 1.0]；FOC 电流环消费） */
    float i_trip_a;                 /**< 快速过流跳闸 [A]（峰值口径；0 = 关闭） */
    float speed_max_rad_s;          /**< FOC 内部连续限速上限 [rad/s 电角]（0 = 关闭；与 motion 限速独立） */
    float motion_iq_max_a;          /**< motion 命令电流硬限制 [A]（力矩命令与速度PI输出上限） */
    float motion_speed_limit_rad_s; /**< 轴端实际速度上限 [rad/s]（0 = 不限；同时约束命令） */
} app_software_limits_t;

/**
 * @brief 电流环参数
 */
typedef struct {
    float kp;              /**< 比例增益 [V/A] */
    float ki;              /**< 积分增益 [V/(A·s)] */
    float bandwidth_rad_s; /**< 目标带宽 [rad/s]（自整定用） */
    uint8_t decoupling_en; /**< 解耦前馈（0/1） */
} app_software_current_loop_t;

/**
 * @brief 控制环参数
 */
typedef struct {
    app_software_current_loop_t current_loop;   /**< 电流环 */
    app_software_speed_loop_t speed_loop;       /**< 速度环 */
    app_software_position_loop_t position_loop; /**< 位置环 */
    app_software_limits_t limits;               /**< 控制限幅 */
} app_software_control_t;

/**
 * @brief 速度/ωe 观测器参数（P0-B；app_foc init 期消费）
 */
typedef struct {
    uint8_t enable;          /**< 1 = 30kHz observer 运行；0 = 仅 foc_angle 回退 */
    float bandwidth_rad_s;   /**< PLL 带宽 [rad/s] */
    float max_speed_rad_s;   /**< 机械转速限幅 [rad/s] */
} app_software_speed_observer_t;

/**
 * @brief 运动用户坐标参数（app_motion init 期消费）
 */
typedef struct {
    float output_direction; /**< 出轴正方向：+1 或 -1；仅 motion 用户坐标，不改 FOC 换相 */
} app_software_motion_t;

/**
 * @brief 电流环前馈参数（追加字段；app_foc_current 每拍 live 消费）
 */
typedef struct {
    uint8_t r_en;    /**< 1 = 启用 R·I 前馈；默认 0（需先通过 cal rl step） */
    uint8_t wl_en;   /**< 1 = 启用 ωL 交叉耦合前馈；默认 0 */
    uint8_t bemf_en; /**< 1 = 启用 bEMF（ωλ）前馈；默认 0 */
} app_software_current_ff_t;

/**
 * @brief motion 输入整形参数（追加字段；app_motion init 期消费）
 */
typedef struct {
    float vel_ramp_rate;    /**< 速度目标斜坡 [rad/s²]（0 = 直通） */
    float torque_ramp_rate; /**< 力矩目标斜坡 [Nm/s]（0 = 直通） */
} app_software_motion_input_t;

/**
 * @brief 位置梯形轨迹参数（追加字段；app_motion init 期消费）
 */
typedef struct {
    float trajectory_vel_limit;   /**< 轨迹最大速度 [rad/s]（0 = 关闭轨迹） */
    float trajectory_accel_limit; /**< 轨迹加速度/减速度 [rad/s²]（0 = 关闭轨迹） */
} app_software_position_traj_t;

/**
 * @brief 齿槽/传动补偿默认启用参数（追加字段；boot / cal apply 时消费）
 */
typedef struct {
    uint8_t enable;        /**< 1 = boot 时默认打开补偿总开关（需有效表） */
    uint8_t rotor_enable;  /**< 1 = 默认启用 rotor 表 */
    uint8_t output_enable; /**< 1 = 默认启用 output 表 */
    uint8_t reserved;      /**< 对齐保留 */
    float gain;            /**< 默认补偿增益（0~2） */
} app_software_cogging_t;

/** @brief flash 参数加载策略 */
typedef enum {
    APP_SW_STORAGE_KEEP = 0,     /**< 使用 flash 中已保存的参数 */
    APP_SW_STORAGE_RESET = 1,    /**< 忽略 flash；SPRM/MPRM 回 YAML，FCUR 清除 */
    APP_SW_STORAGE_OVERRIDE = 2, /**< 本次启动仅 SPRM 使用 YAML 默认，不写回 flash */
} app_sw_storage_t;

/**
 * @brief 软件参数（工厂默认）
 *
 * @note flash 迁移约定：新增字段只能追加在结构体末尾（现有字段偏移保持不变）。
 *       app_software_params_load() 接受旧长度的前缀记录，旧字段照常叠加，
 *       新增字段保持出厂默认，避免升级时静默丢失已保存的运动整定。
 */
typedef struct {
    app_software_can_t can;         /**< CAN 通信 */
    app_software_fault_t fault;     /**< 故障保护 */
    app_software_control_t control; /**< 控制环 */
    uint32_t storage; /**< flash 策略：keep=用flash；reset=SPRM/MPRM回YAML并清FCUR；override=仅SPRM本次用YAML */
    app_software_speed_observer_t speed_observer; /**< P0-B 速度/ωe 观测器（追加字段） */
    float current_loop_output_delay_s; /**< 反 Park 输出延迟补偿 [s]（追加字段；0=关闭） */
    app_software_motion_t motion;      /**< 运动用户坐标（追加字段） */
    app_software_current_ff_t current_ff; /**< 电流环前馈（追加字段） */
    app_software_motion_input_t motion_input; /**< 速度/力矩目标斜坡（追加字段） */
    app_software_position_traj_t position_traj; /**< 位置梯形轨迹（追加字段） */
    float motion_speed_limit_gain; /**< 轴端连续速度限幅增益 [Nm/(rad/s)]（追加字段；0=关闭） */
    float foc_speed_limit_gain; /**< FOC 内部连续限速增益 [A/(rad/s)]（追加字段；0=关闭） */
    float i_q_continuous_a; /**< 电流连续限幅 [A_peak]（追加字段；i_q_max_a 为 boost/硬上限） */
    float i_q_boost_time_s; /**< 30A/boost 满额可持续时间 [s]（追加字段；0=禁用 boost） */
    float i_q_boost_recover_time_s; /**< boost 从耗尽回满时间 [s]（追加字段；0=不回充） */
    float i_q_boost_recover_ratio; /**< 低于连续值×该比例才回充（追加字段，0~1） */
    app_software_cogging_t cogging; /**< 齿槽补偿默认启用/表选择/增益（追加字段） */
} app_software_params_t;

/** @brief 工厂默认参数（只读，指向生成常量） */
const app_software_params_t* app_software_params_default(void);

/** @brief 加载参数：工厂默认 + flash 记忆覆盖（代次/长度不符时仅出厂值）
 * @return 0 = 策略执行完成（keep 有效记录已叠加，或 reset/override 主动跳过 flash）；
 *         -1 = 参数非法/flash 读取失败，out 保持工厂默认 */
int app_software_params_load(app_software_params_t* out);

/** @brief 持久化当前参数到 flash（"SPRM"；停机窗口由调用方保证）；0成功 */
int app_software_params_store(void);

/** @brief 初始化运行期单例（boot 时调用一次；幂等） */
void app_software_params_init(void);

/** @brief 运行期参数单例（只读；消费者统一经此读取；未初始化时自动初始化） */
const app_software_params_t* app_software_params_current(void);

/** @brief 运行期参数单例（可写；仅 Shell param 命令等调试路径使用） */
app_software_params_t* app_software_params_mutable(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_SW_PARAMS_H */
