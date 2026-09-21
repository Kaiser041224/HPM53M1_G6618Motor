/**
 * @file    app_motor_params.h
 * @brief   电机机械/电磁参数（工厂默认，来源 config/motor.yaml）
 * @author  Kaiser
 *
 * 消费者：FOC（后续）。YAML 为出厂初值；将来在线辨识结果经 flash 覆盖（load 内叠加）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_MOTOR_PARAMS_H
#define APP_MOTOR_PARAMS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 编码器齿系与电角度标定（转子轴外齿圈 + 两路小齿轮）
 */
typedef struct {
    uint8_t resolution_bits;     /**< 单圈绝对分辨率 [bit] */
    uint8_t rotor_ring_teeth;    /**< 转子轴外齿圈齿数（两路小齿轮共用） */
    uint8_t rotor_pinion_teeth;  /**< 转子编码器小齿轮齿数 */
    uint8_t output_pinion_teeth; /**< 出轴编码器小齿轮齿数 */
    float   rotor_ratio;         /**< 转子编码器转角 / 转子转角 */
    float   output_ratio;        /**< 出轴编码器转角 / 转子转角 */
    float   electrical_offset_rad; /**< 电角度零点 [rad]（cal encoder 写入） */
    float   direction;             /**< 编码器方向（+1.0 / −1.0） */
} app_motor_encoder_t;

/**
 * @brief 电机机械/电磁参数（工厂默认）
 */
typedef struct {
    uint8_t  pole_pairs;         /**< 极对数 */
    float    rs_ohm;             /**< 相电阻 [Ω] */
    float    ls_h;               /**< 相电感 [H] */
    float    ke_vs_per_rad;      /**< 反电动势系数 [V·s/rad] */
    float    kt_nm_per_a;        /**< 转矩系数 [N·m/A] */
    float    i_rated_a;          /**< 额定电流 [A]（RMS，105°C） */
    float    i_peak_10s_a;       /**< 峰值电流 10s [A]（峰值） */
    float    i_peak_2s_a;        /**< 峰值电流 2s [A]（峰值） */
    float    vbus_nom_v;         /**< 母线额定电压 [V] */
    uint16_t rpm_max;            /**< 最高转速 [rpm] */
    float    inertia_kgm2;       /**< 转动惯量 [kg·m²] */
    float    torque_rated_nm;    /**< 额定转矩 [N·m] */
    float    torque_peak_10s_nm; /**< 峰值转矩 10s [N·m] */
    app_motor_encoder_t encoder; /**< 编码器齿系与电角度标定 */
} app_motor_params_t;

/**
 * @brief  工厂默认参数（只读，指向生成常量）
 * @return 指向工厂常量结构体的只读指针
 */
const app_motor_params_t *app_motor_params_default(void);

/**
 * @brief 加载参数：工厂默认 +（将来）flash 覆盖（在线辨识结果）
 * @param out 输出参数结构体（不可为 NULL）
 */
void app_motor_params_load(app_motor_params_t *out);

/**
 * @brief 初始化运行期单例（boot 时调用一次；幂等）。
 */
void app_motor_params_init(void);

/**
 * @brief 运行期参数单例（只读；消费者统一经此读取）。
 * @return 单例指针（未初始化时自动初始化）
 */
const app_motor_params_t *app_motor_params_current(void);

/**
 * @brief 运行期参数单例（可写；仅 Shell param 命令等调试路径使用）。
 * @return 单例指针
 */
app_motor_params_t *app_motor_params_mutable(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_MOTOR_PARAMS_H */
