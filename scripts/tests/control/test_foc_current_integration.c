/**
 * @file    test_foc_current_integration.c
 * @brief   app_foc_current 主机 mock 集成测试（PI 限幅 / vbus 保护 / vtest 跳闸与计时）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

#include "app_foc.h"
#include "app_foc_current.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "foc_math.h"

#include <math.h>

extern void mock_params_reset(void);
extern void mock_set_current_gains(float kp, float ki);
extern void mock_set_i_trip(float a);
extern float g_mock_duty[3];
extern int g_mock_duty_calls;

/**
 * @brief 三相占空比均落在采样窗口内
 */
static bool duties_in_window(float duty_max) {
    for (int i = 0; i < 3; i++) {
        if ((g_mock_duty[i] > duty_max + 1e-5f) || (g_mock_duty[i] < (1.0f - duty_max - 1e-5f))) {
            return false;
        }
    }
    return true;
}

void test_foc_current_integration(void) {
    app_foc_current_snapshot_t snap;
    float duty[3];
    bool sat;
    int rc;

    mock_params_reset();
    app_foc_current_init();
    CHECK(app_foc_current_is_ready());

    /* 0) 控制层有效占空比上限：保守 0.70（不随用户 0.885 放开） */
    CHECK_NEAR(app_foc_current_duty_max(), 0.885f, 1e-6f); /* mock user=.885，默认ceiling=.95 */

    /* 1) 零矢量命令 */
    app_foc_current_zero_vector();
    CHECK_NEAR(g_mock_duty[0], 0.5f, 1e-6f);
    CHECK_NEAR(g_mock_duty[1], 0.5f, 1e-6f);
    CHECK_NEAR(g_mock_duty[2], 0.5f, 1e-6f);

    /* 2) 零给定零反馈：不饱和、快照有效、占空比在窗口内 */
    app_foc_current_reset();
    rc = app_foc_current_run_fresh(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 24.0f, duty, &sat);
    CHECK(rc == 0);
    CHECK(sat == false);
    CHECK(duties_in_window(app_foc_current_duty_max()));
    app_foc_current_get_snapshot(&snap);
    CHECK(snap.valid == true);
    CHECK_NEAR(snap.v_bus_v, 24.0f, 1e-3f);

    /* 2b) R·I 前馈：默认关闭；打开后 vq 增加 R×iq_ref */
    mock_set_current_gains(1.0f, 0.0f);
    app_motor_params_mutable()->rs_ohm = 0.25f;
    app_software_params_mutable()->current_ff.r_en = 0U;
    app_foc_current_init();
    app_foc_current_reset();
    rc = app_foc_current_run_fresh(0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 24.0f, duty, &sat);
    CHECK(rc == 0);
    app_foc_current_get_snapshot(&snap);
    CHECK_NEAR(snap.v_q_v, 2.0f, 1e-5f);
    app_software_params_mutable()->current_ff.r_en = 1U;
    app_foc_current_reset();
    rc = app_foc_current_run_fresh(0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 24.0f, duty, &sat);
    CHECK(rc == 0);
    app_foc_current_get_snapshot(&snap);
    CHECK_NEAR(snap.v_q_v, 2.5f, 1e-5f);
    app_software_params_mutable()->current_ff.r_en = 0U;

    /* 2c) ωL / bEMF 独立开关：只开 ωL 不叠加 bEMF；只开 bEMF 不叠加 ωL */
    mock_set_current_gains(0.0f, 0.0f);
    app_motor_params_mutable()->rs_ohm = 0.25f;
    app_motor_params_mutable()->ls_h = 1.0e-4f;
    /* 故意把旧 flux 字段置 0：FOC 必须从 Kt_peak/(1.5p) 派生 λ。 */
    app_motor_params_mutable()->kt_nm_per_a = 1.5f;
    app_motor_params_mutable()->flux_linkage_wb = 0.0f;
    app_software_params_mutable()->control.current_loop.decoupling_en = 0U;
    app_software_params_mutable()->current_ff.r_en = 0U;
    app_software_params_mutable()->current_ff.wl_en = 1U;
    app_software_params_mutable()->current_ff.bemf_en = 0U;
    app_foc_current_init();
    app_foc_current_reset();
    rc = app_foc_current_run_fresh(0.0f, 100.0f, 2.0f, 3.0f, 2.0f, 1.5980762f, -3.5980762f,
                                  24.0f, duty, &sat);
    CHECK(rc == 0);
    app_foc_current_get_snapshot(&snap);
    CHECK_NEAR(snap.v_d_v, -0.03f, 1e-5f);
    CHECK_NEAR(snap.v_q_v, 0.02f, 1e-5f);

    app_foc_current_reset();
    app_software_params_mutable()->current_ff.wl_en = 0U;
    app_software_params_mutable()->current_ff.bemf_en = 1U;
    rc = app_foc_current_run_fresh(0.0f, 100.0f, 2.0f, 3.0f, 2.0f, 1.5980762f, -3.5980762f,
                                  24.0f, duty, &sat);
    CHECK(rc == 0);
    app_foc_current_get_snapshot(&snap);
    CHECK_NEAR(snap.v_d_v, 0.0f, 1e-5f);
    CHECK_NEAR(snap.v_q_v, 10.0f, 1e-5f);
    app_software_params_mutable()->current_ff.wl_en = 0U;
    app_software_params_mutable()->current_ff.bemf_en = 0U;

    /* 3) 母线过低：保护路径返回 -1，快照不可信 */
    app_foc_current_reset();
    rc = app_foc_current_run_fresh(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 5.0f, duty, &sat);
    CHECK(rc == -1);
    app_foc_current_get_snapshot(&snap);
    CHECK(snap.valid == false);

    /* 4) 大增益 + 大 iq：PI 圆形限幅触发，占空比仍在采样窗口内 */
    mock_set_current_gains(5.0f, 100.0f);
    app_foc_current_reset();
    rc = app_foc_current_run_fresh(0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 0.0f, 12.0f, duty, &sat);
    CHECK(rc == 0);
    CHECK(sat == true); /* vq = kp·10 = 50V >> v_max ≈ 5.33V */
    CHECK(duties_in_window(app_foc_current_duty_max()));
    app_foc_current_get_snapshot(&snap);
    CHECK_NEAR(snap.v_scale, 1.0f, 1e-4f); /* 到达调制前已被 PI 圆限幅 */

    /* 4b) 输出延迟：同一 vdq、不同 ωe → 反 Park 角不同，duty 必须变化 */
    {
        float duty0[3], duty833[3];
        const app_motor_params_t* motor_params = app_motor_params_current();
        /* ωe 已含 encoder.direction；轴端速度 = ωe/(pp×reducer) */
        float expected_speed =
            5236.0f / ((float)motor_params->pole_pairs * motor_params->reducer_ratio);

        mock_set_current_gains(0.3f, 0.0f);
        app_foc_current_set_output_delay(1.0f / 30000.0f);
        app_foc_current_reset();
        rc = app_foc_current_run_fresh(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 24.0f, duty0,
                                       &sat);
        CHECK(rc == 0);
        app_foc_current_reset();
        rc = app_foc_current_run_fresh(0.0f, 5236.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 24.0f,
                                       duty833, &sat);
        CHECK(rc == 0);
        CHECK(duties_in_window(app_foc_current_duty_max()));
        CHECK(fabsf(duty0[0] - duty833[0]) + fabsf(duty0[1] - duty833[1])
                  + fabsf(duty0[2] - duty833[2])
              > 1e-4f);
        app_foc_current_get_snapshot(&snap);
        CHECK_NEAR(snap.speed_out_rad_s, expected_speed, 1e-2f);

        /* direction=-1：正向电角速度仍应发布正向轴端速度。
         * 旧实现再除一次 dir，会给出相反符号，速度环形成正反馈。 */
        app_motor_params_mutable()->encoder.direction = -1.0f;
        app_foc_current_reset();
        rc = app_foc_current_run_fresh(0.0f, 5236.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 24.0f,
                                       duty833, &sat);
        CHECK(rc == 0);
        app_foc_current_get_snapshot(&snap);
        CHECK_NEAR(snap.speed_out_rad_s, expected_speed, 1e-2f);
        app_motor_params_mutable()->encoder.direction = 1.0f;
    }

    /* 5) vtest 过流判断：M1 保护动作全关（software.fault.actions_enable=0）
     *     —— 只判断（rc=-1 + 本拍零矢量回退 + 计数），不锁存跳闸、不停 vtest。
     *     动作加回（开关置 1）后本组断言应恢复为：tripped==true / vtest_active==false。 */
    mock_set_current_gains(0.3f, 100.0f);
    mock_set_i_trip(5.0f);
    app_foc_current_reset();
    CHECK(app_foc_current_vtest_start(1.0f, 0.0f, 0.5f) == 0);
    CHECK(app_foc_current_vtest_active() == true);
    rc = app_foc_current_vtest_step_fresh(10.0f, 0.0f, 0.0f, 24.0f, 0.001f);
    CHECK(rc == -1);
    CHECK(app_foc_current_is_tripped() == false);  /* 只判断：不锁存 */
    CHECK(app_foc_current_vtest_active() == true); /* 只判断：不停 vtest */
    CHECK_NEAR(g_mock_duty[0], 0.5f, 1e-6f); /* 本拍零矢量回退（非停机动作） */

    /* 6) vtest 计时按传入 dt 推进（ADC 实测 dt，与编码器样本无关） */
    mock_set_i_trip(10.0f);
    app_foc_current_reset();
    CHECK(app_foc_current_vtest_start(1.0f, 0.0f, 0.05f) == 0);
    {
        int stopped_at = -1;

        /* dt=5ms（≤ 上限），12 拍 = 60ms > 50ms 必须停止 */
        for (int i = 0; i < 12; i++) {
            rc = app_foc_current_vtest_step_fresh(0.0f, 0.0f, 0.0f, 24.0f, 0.005f);
            if (rc == -1) {
                stopped_at = i;
                break;
            }
        }
        CHECK(stopped_at >= 0);
    }
    CHECK(app_foc_current_vtest_active() == false);
    CHECK_NEAR(g_mock_duty[0], 0.5f, 1e-6f);
}

void test_foc_speed_observer(void) {
    extern float g_mock_rotor_rad;
    extern int g_mock_rotor_fail;
    extern void mock_advance_cycle(uint32_t cycles);
    app_foc_current_snapshot_t snap;

    mock_params_reset();
    app_motor_params_mutable()->pole_pairs = 10U;
    app_motor_params_mutable()->encoder.direction = 1.0f;
    app_motor_params_mutable()->reducer_ratio = 20.0f;
    app_foc_init();
    app_foc_set_speed_observer_enable(true);
    CHECK(app_foc_enable() == 0);

    g_mock_rotor_rad = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        g_mock_rotor_rad = fmodf(g_mock_rotor_rad + 10.0f * (48000.0f / 480000000.0f),
                                 FOC_TWO_PI_F);
        mock_advance_cycle(48000U);
        app_foc_isr_step();
    }
    app_foc_get_snapshot(&snap);
    CHECK_NEAR(snap.omega_e_rad_s, 100.0f, 2.0f);
    CHECK_NEAR(snap.speed_out_rad_s, 0.5f, 0.05f);
    CHECK(snap.speed_out_valid);

    /* FOC 内部连续限速独立生效：电角速度 100rad/s、上限 50、增益 0.1A/(rad/s)
     * → q 电流上限 (50-100)*0.1=-5A；给 +10A 应被改写成 -5A 制动。 */
    app_software_params_mutable()->control.limits.speed_max_rad_s = 50.0f;
    app_software_params_mutable()->foc_speed_limit_gain = 0.1f;
    CHECK(app_foc_set_iq_ref(10.0f) == 0);
    app_foc_run_once();
    app_foc_isr_step();
    app_foc_get_snapshot(&snap);
    /* 限速输出会再被 FOC 电流圆限幅截到 ±i_q_max；按当拍 ωe 计算期望值。 */
    float expected_iq = (50.0f - snap.omega_e_rad_s) * 0.1f;
    if (expected_iq > 10.0f)
        expected_iq = 10.0f;
    else if (expected_iq < -10.0f)
        expected_iq = -10.0f;
    CHECK(snap.i_q_ref_a < 0.0f); /* 超速时只允许制动方向 */
    CHECK_NEAR(snap.i_q_ref_a, expected_iq, 0.2f);
    app_software_params_mutable()->control.limits.speed_max_rad_s = 0.0f;
    app_software_params_mutable()->foc_speed_limit_gain = 0.0f;

    /* 编码器坏帧：observer 速度失效，快照保持有限，不产生 NaN */
    g_mock_rotor_fail = 1;
    mock_advance_cycle(48000U);
    app_foc_isr_step();
    app_foc_get_snapshot(&snap);
    CHECK(isfinite(snap.speed_out_rad_s));
    CHECK(isfinite(snap.omega_e_rad_s));
    g_mock_rotor_fail = 0;
    mock_advance_cycle(48000U);
    app_foc_isr_step(); /* 恢复一帧有效 observer */
    app_foc_get_snapshot(&snap);
    CHECK(snap.speed_out_valid);
    app_foc_disable();
    app_foc_get_snapshot(&snap);
    CHECK(!snap.speed_out_valid); /* 停用后不得保留上一拍 observer 有效标志 */
}
