/**
 * @file    test_foc_vbus_source.c
 * @brief   FOC 母线来源（快通道固定 / NAN 回退）主机自测
 * @author  Kaiser
 *
 * 覆盖目标：
 *   ① **快通道是唯一正式来源**（2026-10-26 配对 A/B 定案：FAST 臂 3/3 通过、887~1445Hz；
 *      SLOW 臂 0/3 通过、卡 754Hz）——快通道可用时必须采用，且 `g_foc_vbus_src_used == 1`；
 *   ② 快通道不可用（无首帧 → NAN）：回退 1kHz 缓存值，`g_foc_vbus_src_used == 0`；
 *   ③ 无粘滞：快通道恢复后立即回到快通道（回退只覆盖"本拍不可用"）。
 *
 * 注：原运行期开关 `g_foc_vbus_fast_en` 已于 2026-10-27 收尾移除，本用例不再覆盖"强制关闭"。
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

    /* ① 快通道可用 → 采用同拍值，来源标记为 1 */
    g_mock_vbus_fast = 23.25f;
    app_foc_isr_step();
    app_foc_get_snapshot(&snap);
    CHECK_NEAR(snap.v_bus_v, 23.25f, 1e-4f);
    CHECK(g_foc_vbus_src_used == 1U);

    /* ② 快通道不可用（NAN）→ 回退 1kHz 缓存值，来源标记为 0 */
    g_mock_vbus_fast = NAN;
    app_foc_isr_step();
    app_foc_get_snapshot(&snap);
    CHECK_NEAR(snap.v_bus_v, 24.0f, 1e-4f);
    CHECK(g_foc_vbus_src_used == 0U);

    /* ③ 无粘滞：快通道恢复 → 立即回到快通道 */
    g_mock_vbus_fast = 22.75f;
    app_foc_isr_step();
    app_foc_get_snapshot(&snap);
    CHECK_NEAR(snap.v_bus_v, 22.75f, 1e-4f);
    CHECK(g_foc_vbus_src_used == 1U);

    /* ④ 复位替身，避免污染后续用例 */
    g_mock_vbus_fast = NAN;
    app_foc_disable();
}