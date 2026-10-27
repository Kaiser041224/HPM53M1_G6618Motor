/**
 * @file    test_foc_vbus_source.c
 * @brief   FOC 母线来源选择（快通道 / 1kHz 缓存 / A/B 开关）主机自测
 * @author  Kaiser
 *
 * 覆盖目标：
 *   ① **默认采用快通道**（2026-10-26 配对 A/B 定案：FAST 臂 3/3 通过、887~1445Hz；
 *      SLOW 臂 0/3 通过、卡 754Hz）；
 *   ② 强制关闭开关：即使快通道可用也用 1kHz 缓存值（A/B 对照组 / 回退手段）；
 *   ③ 开关打开但快通道不可用（无首帧 → NAN）：回退 1kHz 缓存值，无粘滞。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

#include "app_foc.h"
#include "app_foc_current.h"

extern void mock_params_reset(void);
extern float g_mock_vbus_fast;

void test_foc_vbus_fast_source(void) {
    app_foc_current_snapshot_t snap;

    mock_params_reset(); /* 快通道替身复位为 NAN，1kHz 缓存替身为 24V */
    app_foc_init();
    CHECK(app_foc_enable() == 0);

    /* ① 默认即采用快通道（同拍值），无需外部置位 */
    CHECK(g_foc_vbus_fast_en == 1U);
    g_mock_vbus_fast = 23.25f;
    app_foc_isr_step();
    app_foc_get_snapshot(&snap);
    CHECK_NEAR(snap.v_bus_v, 23.25f, 1e-4f);

    /* ② 强制关闭（A/B 对照组）：改用 1kHz 缓存值 */
    g_foc_vbus_fast_en = 0U;
    app_foc_isr_step();
    app_foc_get_snapshot(&snap);
    CHECK_NEAR(snap.v_bus_v, 24.0f, 1e-4f);

    /* ③ 打开但快通道不可用 → 立即回退 1kHz 缓存值（无粘滞） */
    g_foc_vbus_fast_en = 1U;
    g_mock_vbus_fast = NAN;
    app_foc_isr_step();
    app_foc_get_snapshot(&snap);
    CHECK_NEAR(snap.v_bus_v, 24.0f, 1e-4f);

    /* ④ 恢复默认，避免污染后续用例 */
    g_foc_vbus_fast_en = 1U;
    app_foc_disable();
}