/**
 * @file test_current_identify.c
 * @brief 使用真实电流环和RL对象验证正式辨识状态机
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_current_identify.h"
#include "app_foc.h"
#include "app_foc_current.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "test_util.h"
extern void mock_params_reset(void);
extern void mock_param_store_reset(void);
extern int g_mock_param_stores;
extern void mock_set_pwm_hz(uint32_t hz);
extern void mock_set_current_gains(float kp, float ki);
extern float g_mock_rotor_rad;

void test_current_identify(void) {
    mock_params_reset();
    mock_param_store_reset();
    app_foc_init();
    CHECK(app_current_identify_start() != 0);
    CHECK(app_current_identify_apply() == -2);          /* OFF但无完成结果，必须明确区别 */
    CHECK(app_foc_enable() == 0);
    CHECK(app_current_identify_restore()!=0); /* 运行中不得覆盖配置 */
    CHECK(app_current_identify_start() == 0);
    CHECK(app_current_identify_apply() == -3);
    CHECK(app_foc_current_run_fresh(0, 0, 0, 0, 0, 0, 0, 24, NULL, NULL) == 0);
    CHECK_NEAR(g_foc_current_snapshot.v_d_v, 0, 1e-6f); /* 先零电压同PWM状态基线，不先注入2A */
    CHECK(app_current_identify_start() != 0);
    app_current_identify_abort();
    CHECK(!app_current_identify_is_active());
    CHECK(app_foc_get_state() == APP_FOC_STATE_READY);
    CHECK(app_current_identify_start() == 0);
    float current = 0;
    bool rs_edge_injected = false;
    unsigned ld_corrupt_blocks = 0;
    for (unsigned tick = 0; tick < 30000 && app_current_identify_is_active(); ++tick) {
        app_current_identify_result_t live;
        app_current_identify_get_result(&live);
        float ref = live.current_ref_a;
        for (unsigned i = 0; i < 25; ++i) {
            float v = g_foc_current_snapshot.v_d_v;
            current = (current - v / 0.2f) * expf(-0.2f * 40e-6f / 120e-6f) + v / 0.2f;
            CHECK(
                app_foc_current_run_fresh(
                    0, 0, ref, 0, current, -0.5f * current, -0.5f * current, 24, NULL, NULL)
                == 0);
        }
        if (live.state == APP_CURRENT_IDENTIFY_RS && !rs_edge_injected) {
            uint16_t n;
            const id_rl_sample_t* captured = app_foc_current_identify_data(&n);
            if (n == APP_FOC_IDENTIFY_SAMPLES) {
                /* 冻结输入缓冲注入首块偏差；模拟块均值偶发波动，不影响对象状态。 */
                id_rl_sample_t* samples = (id_rl_sample_t*)captured;
                for (unsigned j = 0; j < n; ++j)
                    samples[j].i_d_a += 0.2f;
                rs_edge_injected = true;
            }
        }
        if (live.state == APP_CURRENT_IDENTIFY_LD && ld_corrupt_blocks < 128) {
            uint16_t n;
            id_rl_sample_t* samples = (id_rl_sample_t*)app_foc_current_identify_data(&n);
            if (n == APP_FOC_IDENTIFY_SAMPLES) {
                /* 两批带相反扰动，各自不合格；仅累计全部128块才抵消，不能挑好批。 */
                for (unsigned j = 0; j < n; ++j)
                    samples[j].i_d_a += (ld_corrupt_blocks < 64 ? 1.0f : -1.0f)
                                      * ((j / 4U) % 3U == 0 ? 0.4f : -0.2f) * (j % 4U);
                ++ld_corrupt_blocks;
            }
        }
        app_current_identify_run_once();
        if (live.state == APP_CURRENT_IDENTIFY_LD && app_current_identify_is_active()) {
            app_current_identify_result_t next;
            app_current_identify_get_result(&next);
            if (next.state == APP_CURRENT_IDENTIFY_PI_SETTLE) {
                float origin = g_mock_rotor_rad;
                g_mock_rotor_rad += 0.005f;
                app_foc_isr_step();
                CHECK_NEAR(
                    g_foc_current_snapshot.theta_e_rad, origin * 10.0f,
                    0.001f); /* 整个辨识保持固定激励角 */
                g_mock_rotor_rad = origin;
            }
        }
    }
    app_current_identify_result_t result;
    app_current_identify_get_result(&result);
    CHECK(result.done);
    CHECK(result.ld_extra_batches > 0);
    CHECK_NEAR(result.pi_baseline_a, 3.0f, 0.05f);
    CHECK(result.pi_baseline_rms_a < 0.05f);
    CHECK_NEAR(result.rs_ohm, 0.2f, 0.005f);
    CHECK_NEAR(result.ld_h, 120e-6f, 8e-6f);
    CHECK(result.bandwidth_hz >= 200);
    CHECK_NEAR(result.kp, result.ld_h * 1256.63706f, 1e-5f);
    CHECK_NEAR(result.ki, result.rs_ohm * 1256.63706f, 0.01f);
    CHECK_NEAR(app_motor_params_current()->rs_ohm, 0.158f, 1e-6f);
    CHECK_NEAR(app_software_params_current()->control.current_loop.ki, 100, 1e-6f);
    app_foc_disable();
    CHECK(app_current_identify_save() != 0);
    CHECK(g_mock_param_stores == 0);
    mock_set_pwm_hz(20000);
    CHECK(app_current_identify_apply()==-4);
    mock_set_pwm_hz(25000);
    CHECK(app_current_identify_apply() == 0);
    app_software_params_mutable()->control.current_loop.kp+=0.1f;
    CHECK(app_current_identify_save()!=0);
    app_software_params_mutable()->control.current_loop.kp=result.kp;
    CHECK(app_current_identify_save() == 0);
    CHECK(g_mock_param_stores == 1);
    mock_params_reset();
    CHECK(app_current_identify_restore() == 0);
    CHECK_NEAR(app_motor_params_current()->rs_ohm, result.rs_ohm, 1e-6f);
    mock_set_pwm_hz(20000);
    CHECK(app_current_identify_restore() == -1);
    mock_set_pwm_hz(25000);
    app_foc_init();
    CHECK(app_foc_enable() == 0);
    CHECK(app_current_identify_start() == 0);
    app_foc_disable();
    app_current_identify_run_once();
    app_current_identify_get_result(&result);
    CHECK(result.failed && !result.active);
    /* 宽量程瞬时波动不应直接否决平台；均值明显q偏差仍必须拒绝。 */
    mock_params_reset();
    mock_set_current_gains(0.3f, 0); /* 固定反馈注入不闭合q轴对象，避免积分饱和干扰目标分支 */
    app_foc_init();
    CHECK(app_foc_enable() == 0);
    CHECK(app_current_identify_start() == 0);
    for (unsigned tick = 0; tick < 2000 && app_current_identify_is_active(); ++tick) {
        for (unsigned i = 0; i < 25; ++i)
            app_foc_current_run_fresh(0, 0, 1, 0, 1, -0.2401924f, -0.7598076f, 24, NULL, NULL);
        app_current_identify_run_once();
    }
    app_current_identify_get_result(&result);
    CHECK(result.failed);
    CHECK(result.failure_stage == APP_CURRENT_IDENTIFY_RS);
    CHECK(result.detail == APP_CURRENT_IDENTIFY_DETAIL_Q_CURRENT);
    CHECK(result.capture_count >= 2500);
    app_foc_disable();
    mock_params_reset();
    app_foc_init();
    CHECK(app_foc_enable() == 0);
    CHECK(app_current_identify_start() == 0);
    current = 0;
    for (unsigned tick = 0; tick < 30000 && app_current_identify_is_active(); ++tick) {
        app_current_identify_result_t live;
        app_current_identify_get_result(&live);
        for (unsigned i = 0; i < 25; ++i) {
            float v = g_foc_current_snapshot.v_d_v;
            current = (current - v / 0.2f) * expf(-0.2f * 40e-6f / 120e-6f) + v / 0.2f;
            float feedback = live.state == APP_CURRENT_IDENTIFY_PI ? 3.2f : current;
            app_foc_current_run_fresh(
                0, 0, live.current_ref_a, 0, feedback, -0.5f * feedback, -0.5f * feedback, 24, NULL,
                NULL);
        }
        app_current_identify_run_once();
    }
    app_current_identify_get_result(&result);
    CHECK(result.done && !result.failed && result.reason == 0);
    CHECK(!result.step.passed);      /* 诊断未通过不再等价于模型参数计算失败 */
    CHECK(result.kp > 0 && result.ki > 0);
    CHECK(result.trial_step.error_a > 0.25f);
    CHECK(result.trial_step.rise_s > 0);
    CHECK_NEAR(result.trial_bandwidth_hz, 200, 0.1f);
    CHECK(result.trial_kp > 0 && result.trial_ki > 0);
    app_foc_disable();
    CHECK(app_current_identify_apply() == 0); /* 模型结果可显式应用；不是自动应用 */
    CHECK_NEAR(app_software_params_current()->control.current_loop.kp, result.kp, 1e-6f);
    app_foc_init();
    CHECK(app_foc_enable() == 0);
    app_software_params_mutable()->control.current_loop.bandwidth_rad_s = NAN;
    CHECK(app_current_identify_start() != 0);
    CHECK(app_foc_get_state() == APP_FOC_STATE_READY);
    app_software_params_mutable()->control.current_loop.bandwidth_rad_s = 25000;
    CHECK(app_current_identify_start() != 0);
    app_software_params_mutable()->control.current_loop.bandwidth_rad_s = 6283.185307f;
    CHECK(app_current_identify_start() == 0); /* 25kHz采样下允许1kHz试验 */
    app_current_identify_abort();
    app_software_params_mutable()->control.current_loop.bandwidth_rad_s = 6284.0f;
    CHECK(app_current_identify_start() != 0);
    app_foc_disable();
}
