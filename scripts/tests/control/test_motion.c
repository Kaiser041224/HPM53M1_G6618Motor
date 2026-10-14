/** @file test_motion.c
 * @brief motion静态相位校准、权限与命令测试
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_foc.h"
#include "app_motion.h"
#include "app_motor_params.h"
#include "app_param.h"
#include "test_util.h"
extern void mock_params_reset(void);
void test_motion(void) {
    mock_params_reset();
    app_foc_init();
    app_motion_init();
    app_motor_params_mutable()->kt_nm_per_a = .117f;
    app_motion_step();
    app_motion_snapshot_t before;
    app_motion_get_snapshot(&before);
    CHECK(before.bad_count == 0); /* 未标定不是采样错误 */
    CHECK(app_motion_calibrate() == 0);
    for (unsigned i = 0; i < 1100; ++i)
        app_motion_step();
    app_motion_snapshot_t snapshot;
    app_motion_get_snapshot(&snapshot);
    CHECK(snapshot.calibration_done);
    CHECK(app_motion_apply() == 0);
    app_motion_step();
    CHECK(app_motion_zero() == 0);
    CHECK_NEAR(g_motion_shaft_angle_rad, 0, 1e-6f);
    CHECK_NEAR(g_motion_shaft_angle_deg, 0, 1e-6f);
    CHECK(g_motion_shaft_angle_valid == 1);
    app_motion_get_snapshot(&snapshot);
    CHECK_NEAR(snapshot.shaft_rad, 0, 1e-6f);
    CHECK(app_motion_save() == 0);
    /* Flash恢复不能保留上次RAM增益或活动状态，机械零点仍有效。 */
    app_motion_init();
    app_motion_get_snapshot(&snapshot);
    CHECK(snapshot.calibrated && snapshot.zero_valid && !snapshot.active);
    app_motion_step();
    CHECK(app_motion_apply() != 0);     /* 结果已消费，不可再次作废零点 */
    CHECK(app_motion_calibrate() == 0);
    extern int g_mock_output_failure;
    g_mock_output_failure = 1;
    app_motion_step();
    app_motion_get_snapshot(&snapshot);
    CHECK(!snapshot.calibrating);
    g_mock_output_failure = 0;
    CHECK(app_motion_calibrate() == 0);
    app_motion_get_snapshot(&snapshot);
    CHECK(!snapshot.feedback_valid);
    for (unsigned i = 0; i < 1000; ++i)
        app_motion_step();
    CHECK(app_motion_apply() == 0);
    app_motion_step();
    CHECK(app_foc_enable() == 0);
    CHECK(app_motion_torque(.1f) == 0);
    app_motion_get_snapshot(&snapshot);
    CHECK(snapshot.reason == APP_MOTION_REASON_NONE);
    extern void mock_advance_cycle(uint32_t cycles);
    mock_advance_cycle(48000);
    app_motion_step();
    app_foc_isr_step();
    app_foc_execution_t feedback;
    app_foc_get_execution(&feedback);
    CHECK(feedback.accepted_iq_a > 0);
    app_motor_params_mutable()->kt_nm_per_a = .2f;
    CHECK(app_motion_torque(.1f) != 0); /* 重复命令不得静默接受新的换算基准 */
    app_motor_params_mutable()->kt_nm_per_a = .117f;
    extern int g_mock_output_failure;
    g_mock_output_failure = 1;
    app_motion_step();
    app_motion_get_snapshot(&snapshot);
    CHECK(!snapshot.active);
    CHECK(snapshot.reason == APP_MOTION_REASON_SAMPLE);
    CHECK_NEAR(snapshot.iq_a, 0, 1e-6f);
    CHECK_NEAR(snapshot.torque_nm, 0, 1e-6f);
    CHECK(g_motion_shaft_angle_valid == 0);
    CHECK(snapshot.sample_failure_count > 0);
    g_mock_output_failure = 0;
    CHECK(app_motion_calibrate() != 0);
    CHECK(app_motion_stop() == 0);
    app_motion_get_snapshot(&snapshot);
    CHECK_NEAR(snapshot.iq_a, 0, 1e-6f);
    CHECK(snapshot.reason == APP_MOTION_REASON_USER_STOP);
    app_foc_disable();
    extern float g_mock_rotor_rad;
    app_motion_step();
    app_motion_get_snapshot(&snapshot);
    uint32_t bad_before = snapshot.bad_count;
    g_mock_rotor_rad += .02f;           /* 轴端约4rad/s，不得误判为角度跳变 */
    mock_advance_cycle(48000);
    app_motion_step();
    app_motion_get_snapshot(&snapshot);
    CHECK(snapshot.feedback_valid);
    CHECK(snapshot.bad_count == bad_before);
    mock_advance_cycle(480000);
    CHECK(app_motion_zero() != 0);      /* ISR中止时旧valid不能作为当前数据 */
    app_motion_step();
    app_motor_params_mutable()->encoder.direction = -1;
    CHECK(app_motion_save() != 0);
    app_motor_params_mutable()->encoder.direction = 1;
    CHECK(app_motion_speed(.1f) != 0);  /* 增益未配置 */
    CHECK(app_motion_gains(.05f, .1f) == 0);
    CHECK(app_foc_enable() == 0);
    app_motion_step();
    CHECK(app_motion_speed(.1f) == 0);
    extern void mock_advance_cycle(uint32_t cycles);
    mock_advance_cycle(48000);
    app_motion_step();
    app_motion_get_snapshot(&snapshot);
    CHECK(snapshot.active);
    CHECK(app_motion_stop() == 0);
    app_motion_get_snapshot(&snapshot);
    CHECK_NEAR(snapshot.torque_nm, 0, 1e-6f);
    app_foc_disable();
    CHECK(app_foc_enable() == 0);
    CHECK(app_foc_calib_begin() == 0);
    CHECK(app_motion_stop() != 0);      /* 不回报成功，也不抢校准输出 */
    CHECK(app_foc_get_state() == APP_FOC_STATE_CALIB);
    app_foc_calib_end();
    app_foc_disable();
}

/** @brief 相同样本间隔下，改变GPTMR入口延迟不应改变测得速度 */
void test_motion_sample_time(void) {
    extern float g_mock_rotor_rad;
    extern bool g_mock_output_timestamp_override;
    extern uint32_t g_mock_output_timestamp_cycles;
    extern void mock_advance_cycle(uint32_t cycles);
    mock_params_reset();
    app_foc_init();
    g_mock_output_timestamp_override = true;
    float measured_speed[2];
    for (unsigned i = 0; i < 2; ++i) {
        app_motion_init();
        g_mock_rotor_rad = 0;
        CHECK(app_motion_calibrate() == 0);
        for (unsigned j = 0; j < 1000; ++j)
            app_motion_step();
        CHECK(app_motion_apply() == 0);
        g_mock_output_timestamp_cycles = 1000000U;
        app_motion_step();
        g_mock_rotor_rad = .01f;
        g_mock_output_timestamp_cycles += 38400U;     /* 两个25kHz拍=80us */
        mock_advance_cycle(i == 0 ? 48000U : 72000U); /* 入口间隔100/150us */
        app_motion_step();
        app_motion_snapshot_t snapshot;
        app_motion_get_snapshot(&snapshot);
        CHECK(snapshot.feedback_valid);
        measured_speed[i] = snapshot.speed_rad_s;
    }
    CHECK_NEAR(measured_speed[0], measured_speed[1], 1e-5f);
    CHECK_NEAR(measured_speed[0], .075f, 1e-4f);      /* .01/50/80us * LPF .03 */
    g_mock_output_timestamp_override = false;
    g_mock_rotor_rad = NAN;
    app_motion_step();
    app_motion_snapshot_t invalid;
    app_motion_get_snapshot(&invalid);
    CHECK(!invalid.feedback_valid && invalid.reason == APP_MOTION_REASON_SAMPLE);
    g_mock_rotor_rad = 0;
}

/** @brief 有CRC但语义非法的MOTN记录也不得用于控制 */
void test_motion_restore_validation(void) {
    extern void mock_param_store_reset(void);
    mock_params_reset();
    app_foc_init();
    struct {
        uint32_t version;
        float phase, zero, direction;
        uint32_t zero_valid;
    } record = {1, .01f, 1e20f, 1, 1};
    CHECK(app_param_store(APP_PARAM_KEY_MOTION, &record, sizeof(record)) == 0);
    app_motion_init();
    app_motion_snapshot_t snapshot;
    app_motion_get_snapshot(&snapshot);
    CHECK(!snapshot.calibrated);
    record.zero = 0;
    record.direction = 0;
    app_motor_params_mutable()->encoder.direction = 0;
    CHECK(app_param_store(APP_PARAM_KEY_MOTION, &record, sizeof(record)) == 0);
    app_motion_init();
    app_motion_get_snapshot(&snapshot);
    CHECK(!snapshot.calibrated);
    mock_param_store_reset();
    app_motor_params_mutable()->encoder.direction = 1;
}

/** @brief 编码器跨圈与负方向坐标不应引入轴端7.2deg跳变 */
void test_motion_wrap_direction(void) {
    extern float g_mock_rotor_rad;
    extern bool g_mock_output_timestamp_override, g_mock_output_raw_override;
    extern uint32_t g_mock_output_timestamp_cycles;
    extern uint16_t g_mock_output_raw;
    extern void mock_param_store_reset(void);
    mock_param_store_reset();
    mock_params_reset();
    app_foc_init();
    struct {
        uint32_t version;
        float phase, zero, direction;
        uint32_t zero_valid;
    } record = {1, .007f, 0, -1, 0};
    app_motor_params_mutable()->encoder.direction = -1;
    CHECK(app_param_store(APP_PARAM_KEY_MOTION, &record, sizeof(record)) == 0);
    app_motion_init();
    g_mock_output_raw_override = true;
    g_mock_output_timestamp_override = true;
    g_mock_rotor_rad = .9999f * 6.283185307f;
    g_mock_output_raw = 64678U;
    g_mock_output_timestamp_cycles = 1000000U;
    app_motion_step();
    app_motion_snapshot_t first;
    app_motion_get_snapshot(&first);
    g_mock_rotor_rad = .0001f * 6.283185307f;
    g_mock_output_raw = 64687U;
    g_mock_output_timestamp_cycles += 38400;
    app_motion_step();
    app_motion_snapshot_t second;
    app_motion_get_snapshot(&second);
    CHECK(first.feedback_valid && second.feedback_valid);
    CHECK(second.speed_rad_s < 0);
    CHECK(fabsf(second.shaft_rad - first.shaft_rad) < .0001f);
    g_mock_output_raw_override = false;
    g_mock_output_timestamp_override = false;
    g_mock_rotor_rad = 0;
    app_motor_params_mutable()->encoder.direction = 1;
    mock_param_store_reset();
}
