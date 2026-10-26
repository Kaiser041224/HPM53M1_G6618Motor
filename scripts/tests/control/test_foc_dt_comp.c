/**
 * @file    test_foc_dt_comp.c
 * @brief   逆变器死区补偿主机 mock 集成测试（折算 / 符号滞回 / 关闭语义）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

#include "app_foc.h"
#include "app_foc_current.h"

extern void mock_params_reset(void);
extern void mock_set_deadtime_comp_ns(uint32_t ns);
extern float g_mock_duty[3];

void test_foc_dt_comp(void) {
    app_foc_current_snapshot_t snap;

    mock_params_reset();
    app_foc_init();

    /* 默认（0ns）关闭：折算占空比 0，输出与无补偿路径一致 */
    CHECK_NEAR(app_foc_current_dt_comp_duty(), 0.0f, 1e-9f);
    CHECK(app_foc_enable() == 0);
    CHECK(app_foc_current_run_fresh(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 24.0f, NULL, NULL)
          == 0);
    CHECK_NEAR(g_mock_duty[0], 0.5f, 1e-6f);
    CHECK_NEAR(g_mock_duty[1], 0.5f, 1e-6f);
    CHECK_NEAR(g_mock_duty[2], 0.5f, 1e-6f);

    /* 150ns @ 25kHz(mock) → 0.00375 duty；reload_model 折算（foc on 期同路径） */
    mock_set_deadtime_comp_ns(150U);
    CHECK(app_foc_current_reload_model() == 0);
    CHECK_NEAR(app_foc_current_dt_comp_duty(), 0.00375f, 1e-7f);

    /* 共模三相电流（Clarke=0 → PI 误差 0 → 电压指令 0）：
     * 电流 > +0.3A → 三相符号 +1 → duty = 0.5 + comp */
    CHECK(app_foc_current_run_fresh(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 24.0f, NULL, NULL)
          == 0);
    CHECK_NEAR(g_mock_duty[0], 0.5f + 0.00375f, 1e-6f);
    CHECK_NEAR(g_mock_duty[1], 0.5f + 0.00375f, 1e-6f);
    CHECK_NEAR(g_mock_duty[2], 0.5f + 0.00375f, 1e-6f);
    /* 快照记录的是实际下发占空比（含补偿） */
    app_foc_current_get_snapshot(&snap);
    CHECK_NEAR(snap.duty_u, g_mock_duty[0], 1e-9f);

    /* 符号滞回：0.1A 在阈值带内 → 保持上一符号（+1），补偿不消失 */
    CHECK(app_foc_current_run_fresh(0.0f, 0.0f, 0.0f, 0.0f, 0.1f, 0.1f, 0.1f, 24.0f, NULL, NULL)
          == 0);
    CHECK_NEAR(g_mock_duty[0], 0.5f + 0.00375f, 1e-6f);

    /* 电流反向 → 符号 -1 → duty = 0.5 − comp */
    CHECK(app_foc_current_run_fresh(0.0f, 0.0f, 0.0f, 0.0f, -1.0f, -1.0f, -1.0f, 24.0f, NULL,
                                    NULL)
          == 0);
    CHECK_NEAR(g_mock_duty[0], 0.5f - 0.00375f, 1e-6f);
    CHECK_NEAR(g_mock_duty[1], 0.5f - 0.00375f, 1e-6f);

    /* 误配置防护：5000ns @ 25kHz = 0.125 → 钳到 2% 上限 */
    mock_set_deadtime_comp_ns(5000U);
    CHECK(app_foc_current_reload_model() == 0);
    CHECK_NEAR(app_foc_current_dt_comp_duty(), 0.02f, 1e-7f);
    CHECK(app_foc_current_run_fresh(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 24.0f, NULL, NULL)
          == 0);
    CHECK_NEAR(g_mock_duty[0], 0.52f, 1e-6f);

    /* 补偿后占空比仍落在采样窗口内 */
    for (int i = 0; i < 3; i++) {
        CHECK(g_mock_duty[i] <= 0.885f + 1e-5f);
        CHECK(g_mock_duty[i] >= 1.0f - 0.885f - 1e-5f);
    }

    /* 归零恢复关闭语义 */
    mock_set_deadtime_comp_ns(0U);
    CHECK(app_foc_current_reload_model() == 0);
    CHECK(app_foc_current_run_fresh(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 24.0f, NULL, NULL)
          == 0);
    CHECK_NEAR(g_mock_duty[0], 0.5f, 1e-6f);

    /* CALIB（cal rl / cal encoder / motor id）挂起补偿：辨识守卫对补偿设置免疫。
     * 机理：固定符号模式的补偿矢量在 q 轴投影为直流电压 → Q_CURRENT 误判。 */
    mock_set_deadtime_comp_ns(150U);
    CHECK(app_foc_current_reload_model() == 0);
    CHECK_NEAR(app_foc_current_dt_comp_duty(), 0.00375f, 1e-7f);
    CHECK(app_foc_calib_begin() == 0);
    CHECK(app_foc_current_run_fresh(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 24.0f, NULL, NULL)
          == 0);
    CHECK_NEAR(g_mock_duty[0], 0.5f, 1e-6f);
    CHECK_NEAR(g_mock_duty[1], 0.5f, 1e-6f);
    CHECK_NEAR(g_mock_duty[2], 0.5f, 1e-6f);
    app_foc_calib_end();

    /* 退出 CALIB 后恢复补偿（READY 态） */
    CHECK(app_foc_current_run_fresh(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 24.0f, NULL, NULL)
          == 0);
    CHECK_NEAR(g_mock_duty[0], 0.5f + 0.00375f, 1e-6f);

    app_foc_disable();
}
