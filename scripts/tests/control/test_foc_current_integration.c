/**
 * @file    test_foc_current_integration.c
 * @brief   app_foc_current 主机 mock 集成测试（PI 限幅 / vbus 保护 / vtest 跳闸与计时）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

#include "app_foc_current.h"

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
    CHECK_NEAR(app_foc_current_duty_max(), 0.70f, 1e-6f);

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

    /* 5) vtest 过流判断：M1 保护动作全关（app_protect_policy.h APP_PROTECT_ACTION_EN=0）
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
