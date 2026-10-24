/**
 * @file    test_friction_identify.c
 * @brief   摩擦辨识状态机测试（含摩擦被控对象仿真）
 * @author  Kaiser
 *
 * 被控对象：静摩擦 0.6Nm + 粘性 0.2Nm/(rad/s)；力矩不足不转，超出后速度线性。
 * 稳态下 motion 力矩请求 = 摩擦 → 拟合应恢复 Tc≈0.6 / Kv≈0.2。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_foc.h"
#include "app_friction_identify.h"
#include "app_motion.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "test_util.h"

#include <math.h>

extern void mock_params_reset(void);
extern void mock_advance_cycle(uint32_t cycles);
extern float g_mock_rotor_rad;

#define PLANT_COULOMB        (0.6f)
#define PLANT_VISCOUS        (0.2f)
#define PLANT_TWO_PI         (6.283185307179586)
#define PLANT_VERNIER_OFFSET (0.007f)

static double s_plant_rotor_abs_rad; /**< 绝对转子角（游标原码需单圈取整建模） */
static float s_plant_coulomb_pos = PLANT_COULOMB;
static float s_plant_coulomb_neg = PLANT_COULOMB;

static void plant_sensor_update(float* rotor_rad) {
    extern bool g_mock_output_raw_override;
    extern uint16_t g_mock_output_raw;
    *rotor_rad = (float)fmod(s_plant_rotor_abs_rad, PLANT_TWO_PI);
    g_mock_output_raw_override = true;
    g_mock_output_raw =
        (uint16_t)(fmod(0.98 * s_plant_rotor_abs_rad / PLANT_TWO_PI + PLANT_VERNIER_OFFSET, 1.0)
                   * 65536.0);
}

static void plant_step(float* rotor_rad, float dt_s) {
    app_motion_snapshot_t motion;
    app_motion_get_snapshot(&motion);
    float torque = motion.torque_nm;
    float speed = 0;
    if (torque > s_plant_coulomb_pos)
        speed = (torque - s_plant_coulomb_pos) / PLANT_VISCOUS;
    else if (torque < -s_plant_coulomb_neg)
        speed = (torque + s_plant_coulomb_neg) / PLANT_VISCOUS;
    s_plant_rotor_abs_rad += (double)speed * 50.0 * dt_s; /* 转子侧 = 轴端 × reducer(50) */
    plant_sensor_update(rotor_rad);
}

/** @brief 仿真辨识至终态，返回终态 tick 数 */
static unsigned run_identify(unsigned max_ticks) {
    extern void mock_advance_cycle(uint32_t cycles);
    extern float g_mock_rotor_rad;
    extern bool g_mock_output_timestamp_override;
    extern uint32_t g_mock_output_timestamp_cycles;
    extern int g_mock_output_failure;
    app_friction_identify_snapshot_t snapshot;
    unsigned end_tick = max_ticks;

    g_mock_output_timestamp_override = true;
    for (unsigned tick = 0; tick < max_ticks; ++tick) {
        g_mock_output_failure = (tick % 23 == 0); /* 每23拍一次坏样本：motion有界保持应自愈 */
        plant_step(&g_mock_rotor_rad, 1e-4f);
        mock_advance_cycle(48000);
        g_mock_output_timestamp_cycles += 48000U; /* 样本时间戳与仿真步长一致 */
        app_motion_step();
        app_foc_isr_step();
        if (tick % 10 == 0)                       /* 1kHz 辨识节拍（motion 15kHz） */
            app_friction_identify_run_once();
        app_friction_identify_get_snapshot(&snapshot);
        if (snapshot.state == APP_FRICTION_IDENTIFY_DONE
            || snapshot.state == APP_FRICTION_IDENTIFY_FAILED) {
            end_tick = tick;
            break;
        }
    }
    g_mock_output_timestamp_override = false;
    g_mock_output_failure = 0;
    return end_tick;
}

/** @brief 完整辨识流程：进度推进、拟合恢复被控对象摩擦、apply 写透 */
void test_friction_identify(void) {
    extern void mock_param_store_reset(void);
    mock_param_store_reset();
    mock_params_reset();
    s_plant_rotor_abs_rad = 0;
    plant_sensor_update(&g_mock_rotor_rad);     /* 标定前启用游标单圈模型 */
    app_foc_init();
    app_motion_init();
    app_motor_params_mutable()->kt_nm_per_a = .117f;
    CHECK(app_motion_gains(2, 50, 0, 0) == 0);
    CHECK(app_motion_calibrate() == 0);
    for (unsigned i = 0; i < 1000; ++i)
        app_motion_step();
    CHECK(app_motion_apply() == 0);
    app_motion_step();
    app_friction_identify_init();
    app_friction_identify_snapshot_t snapshot;
    app_friction_identify_get_snapshot(&snapshot);
    CHECK(snapshot.state == APP_FRICTION_IDENTIFY_IDLE);
    CHECK(app_friction_identify_start(0) != 0); /* FOC 未使能 */
    CHECK(app_foc_enable() == 0);
    CHECK(app_friction_identify_start(0) == 0);
    CHECK(app_friction_identify_busy());
    /* 目标命令瞬态拒绝（反馈陈旧）→ 重试而非判死 */
    mock_advance_cycle(480000);
    app_friction_identify_run_once();
    app_friction_identify_get_snapshot(&snapshot);
    CHECK(snapshot.state == APP_FRICTION_IDENTIFY_SETTLING);
    CHECK(snapshot.failure == APP_FRICTION_IDENTIFY_FAIL_NONE);
    unsigned done_ticks = run_identify(400000);
    app_friction_identify_get_snapshot(&snapshot);
    CHECK(snapshot.state == APP_FRICTION_IDENTIFY_DONE);
    CHECK(snapshot.failure == APP_FRICTION_IDENTIFY_FAIL_NONE);
    CHECK(snapshot.result_valid);
    CHECK(done_ticks > 220000 && done_ticks < 320000);
    CHECK_NEAR(snapshot.result.coulomb_nm, PLANT_COULOMB, 0.1f);
    CHECK_NEAR(snapshot.result.viscous_nm_per_rad_s, PLANT_VISCOUS, 0.05f);
    CHECK(snapshot.residual_rms_nm < 0.1f);
    /* DONE 保持 FOC（用户要求）；apply 不驱动即可用，写透参数单例 */
    CHECK(app_foc_get_state() != APP_FOC_STATE_OFF);
    CHECK(app_friction_identify_apply() == 0);
    CHECK(
        app_software_params_current()->control.speed_loop.friction_coulomb_nm
        == snapshot.result.coulomb_nm);
    CHECK(
        app_software_params_current()->control.speed_loop.friction_viscous_nm_per_rad_s
        == snapshot.result.viscous_nm_per_rad_s);
    {
        extern bool g_mock_output_raw_override;
        g_mock_output_raw_override = false;
    }
}

/** @brief 失败路径：速度无法达到/运动撤销 → 失败且不落盘 */
void test_friction_identify_failure(void) {
    extern float g_mock_rotor_rad;
    extern bool g_mock_output_raw_override;
    mock_params_reset();
    s_plant_rotor_abs_rad = 0;
    plant_sensor_update(&g_mock_rotor_rad);
    app_foc_init();
    app_motion_init();
    app_motor_params_mutable()->kt_nm_per_a = .117f;
    CHECK(app_motion_gains(2, 50, 0, 0) == 0);
    CHECK(app_motion_calibrate() == 0);
    for (unsigned i = 0; i < 1000; ++i)
        app_motion_step();
    CHECK(app_motion_apply() == 0);
    app_motion_step();
    app_friction_identify_init();
    CHECK(app_foc_enable() == 0);
    /* 被控对象卡死（转子不动）：速度达不到 → 失败不落盘 */
    CHECK(app_friction_identify_start(0) == 0);
    app_friction_identify_snapshot_t snapshot;
    app_friction_identify_get_snapshot(&snapshot);
    for (unsigned tick = 0; tick < 50000; ++tick) {
        mock_advance_cycle(48000);
        app_motion_step();
        app_foc_isr_step();
        if (tick % 10 == 0)
            app_friction_identify_run_once();
        app_friction_identify_get_snapshot(&snapshot);
        if (snapshot.state == APP_FRICTION_IDENTIFY_FAILED)
            break;
    }
    CHECK(snapshot.state == APP_FRICTION_IDENTIFY_FAILED);
    CHECK(snapshot.failure == APP_FRICTION_IDENTIFY_FAIL_NOT_SETTLED);
    CHECK(!snapshot.result_valid);
    CHECK(app_friction_identify_apply() != 0); /* 失败不落盘 */
    /* 运动撤销 → 失败（先清软停挂起） */
    app_foc_disable();
    CHECK(app_foc_enable() == 0);
    CHECK(app_friction_identify_start(0) == 0);
    for (unsigned tick = 0; tick < 3000; ++tick) {
        mock_advance_cycle(48000);
        app_motion_step();
        app_foc_isr_step();
        if (tick % 10 == 0)
            app_friction_identify_run_once();
        if (tick == 100) {
            CHECK(app_motion_stop() == 0);
            app_foc_disable();
        }
        app_friction_identify_get_snapshot(&snapshot);
        if (snapshot.state == APP_FRICTION_IDENTIFY_FAILED)
            break;
    }
    CHECK(snapshot.state == APP_FRICTION_IDENTIFY_FAILED);
    CHECK(snapshot.failure == APP_FRICTION_IDENTIFY_FAIL_MOTION);
    g_mock_output_raw_override = false;
}

/** @brief 门限拒绝保留拟合值；容差可调（对称性超限场景） */
void test_friction_identify_tolerance(void) {
    extern float g_mock_rotor_rad;
    s_plant_coulomb_pos = 0.6f;
    s_plant_coulomb_neg = 0.4f;                     /* 正反不对称 → 默认门限应拒 */
    mock_params_reset();
    s_plant_rotor_abs_rad = 0;
    plant_sensor_update(&g_mock_rotor_rad);
    app_foc_init();
    app_motion_init();
    app_motor_params_mutable()->kt_nm_per_a = .117f;
    CHECK(app_motion_gains(2, 50, 0, 0) == 0);
    CHECK(app_motion_calibrate() == 0);
    for (unsigned i = 0; i < 1000; ++i)
        app_motion_step();
    CHECK(app_motion_apply() == 0);
    app_motion_step();
    app_friction_identify_init();
    CHECK(app_foc_enable() == 0);
    CHECK(app_friction_identify_start(0.30f) == 0); /* 显式紧门限触发对称性拒绝 */
    unsigned done_ticks = run_identify(400000);
    (void)done_ticks;
    app_friction_identify_snapshot_t snapshot;
    app_friction_identify_get_snapshot(&snapshot);
    CHECK(snapshot.state == APP_FRICTION_IDENTIFY_FAILED);
    CHECK(snapshot.failure == APP_FRICTION_IDENTIFY_FAIL_ASYMMETRY);
    /* 被拒也必须留下拟合值与质量数据供检视 */
    CHECK_NEAR(snapshot.result.coulomb_nm, 0.5f, 0.15f);
    CHECK(snapshot.residual_rms_nm >= 0);
    CHECK(snapshot.asymmetry_nm > 0);
    CHECK(!snapshot.result_valid);
    CHECK(app_friction_identify_apply() != 0);
    /* 容差放宽（50%）→ 通过 */
    app_foc_disable();
    CHECK(app_foc_enable() == 0);
    CHECK(app_friction_identify_start(0.5f) == 0);
    done_ticks = run_identify(400000);
    app_friction_identify_get_snapshot(&snapshot);
    CHECK(snapshot.state == APP_FRICTION_IDENTIFY_DONE);
    CHECK(snapshot.result_valid);
    CHECK(snapshot.progress > 0.99f);
    CHECK(snapshot.elapsed_ms > 20000);
    CHECK(app_friction_identify_start(2.0f) != 0); /* 门限非法拒绝 */
    /* 默认门限钉：asym≈24% 数据集——默认(35%)通过、显式0.20拒绝（防默认值替换脱靶） */
    s_plant_coulomb_pos = 0.6f;
    s_plant_coulomb_neg = 0.45f;
    app_foc_disable();
    CHECK(app_foc_enable() == 0);
    CHECK(app_friction_identify_start(0.20f) == 0);
    (void)run_identify(400000);
    app_friction_identify_get_snapshot(&snapshot);
    CHECK(snapshot.state == APP_FRICTION_IDENTIFY_FAILED); /* 24% > 20% */
    CHECK(snapshot.failure == APP_FRICTION_IDENTIFY_FAIL_ASYMMETRY);
    app_foc_disable();
    CHECK(app_foc_enable() == 0);
    CHECK(app_friction_identify_start(0) == 0);            /* 默认 35% 应通过 */
    (void)run_identify(400000);
    app_friction_identify_get_snapshot(&snapshot);
    CHECK(snapshot.state == APP_FRICTION_IDENTIFY_DONE);
    app_foc_disable();
    s_plant_coulomb_pos = PLANT_COULOMB;
    s_plant_coulomb_neg = PLANT_COULOMB;
}

/** @brief 诊断与挂起：start分类返回、FF挂起、目标命令持续被拒=FAIL_COMMAND */
void test_friction_identify_diagnostics(void) {
    extern float g_mock_rotor_rad;
    mock_params_reset();
    s_plant_rotor_abs_rad = 0;
    plant_sensor_update(&g_mock_rotor_rad);
    app_foc_init();
    app_motion_init();
    app_motor_params_mutable()->kt_nm_per_a = .117f;
    CHECK(app_motion_gains(2, 50, .5f, 0) == 0);
    /* 负 Kv（Stribeck 拟合斜率）合法；越界拒绝 */
    CHECK(app_motion_gains(2, 50, 1.0f, -0.3f) == 0);
    CHECK(app_motion_gains(2, 50, 1.0f, -101.0f) == APP_MOTION_CMD_BAD_PARAM);
    CHECK(app_motion_gains(2, 50, 1.0f, 0.3f) == 0);
    CHECK(app_motion_calibrate() == 0);
    for (unsigned i = 0; i < 1000; ++i)
        app_motion_step();
    CHECK(app_motion_apply() == 0);
    app_motion_step();
    app_friction_identify_init();
    CHECK(app_friction_identify_start(0) == -4);    /* FOC 未使能 */
    CHECK(app_foc_enable() == 0);
    CHECK(app_friction_identify_start(2.0f) == -1); /* 门限非法 */
    /* FF 挂起：速度模式输出不含摩擦前馈（P=2*0.3=0.6，积分冻结） */
    app_motion_friction_suspend(true);
    CHECK(app_motion_speed(.3f) == 0);
    CHECK(app_friction_identify_start(0) == -3); /* motion 活动中 */
    mock_advance_cycle(48000);
    app_motion_step();
    app_foc_isr_step();
    app_motion_snapshot_t motion;
    app_motion_get_snapshot(&motion);
    CHECK_NEAR(motion.torque_nm, 0.6f, 0.05f);
    CHECK(app_motion_stop() == 0);
    app_foc_disable();
    app_motion_friction_suspend(false);
    /* 目标命令持续被拒（反馈陈旧且不步进）→ FAIL_COMMAND 且重试计数可见 */
    CHECK(app_foc_enable() == 0);
    CHECK(app_friction_identify_start(0) == 0);
    mock_advance_cycle(4800000); /* 反馈过期：目标命令必拒 */
    for (unsigned tick = 0; tick < 300; ++tick) {
        mock_advance_cycle(48000);
        app_friction_identify_run_once();
    }
    app_friction_identify_snapshot_t snapshot;
    app_friction_identify_get_snapshot(&snapshot);
    CHECK(snapshot.state == APP_FRICTION_IDENTIFY_FAILED);
    CHECK(snapshot.failure == APP_FRICTION_IDENTIFY_FAIL_COMMAND);
    CHECK(snapshot.command_retries > 50);
    app_foc_disable();
}
