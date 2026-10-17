/** @file test_motion_timer.c
 * @brief 定时平台配置与观测测试
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_gptmr.h"
#include "app_motion_timer.h"
#include "intf_gptmr.h"
#include "test_util.h"
extern intf_gptmr_cfg_t g_mock_timer_config[16];
extern bool g_mock_timer_running[16];
static void timer_callback(void) {}
void test_motion_timer(void) {
    app_gptmr_init();
    CHECK(app_gptmr_set_frequency(APP_GPTMR_CH_0, 5000) == 0);
    CHECK(app_gptmr_register_callback(APP_GPTMR_CH_0, timer_callback) == 0);
    CHECK(g_mock_timer_config[4].frequency_hz == 5000);
    CHECK(app_gptmr_register_callback(APP_GPTMR_CH_0, NULL) == 0);
    CHECK(g_mock_timer_config[4].callback == NULL);
    CHECK(app_motion_timer_init() == 0);
    CHECK(g_mock_timer_config[4].frequency_hz == 5000);
    CHECK(g_mock_timer_config[5].frequency_hz == 10000);
    CHECK(g_mock_timer_config[4].mode == INTF_GPTMR_MODE_TIMER);
    CHECK(g_mock_timer_config[5].mode == INTF_GPTMR_MODE_TIMER);
    CHECK(!g_mock_timer_running[4]);
    CHECK(app_motion_timer_start() == 0);
    CHECK(g_mock_timer_running[4] && g_mock_timer_running[5]);
    CHECK(!g_mock_timer_running[6] && !g_mock_timer_running[7]);
    g_mock_timer_config[4].callback();
    CHECK(g_mock_timer_config[5].callback != NULL);
    if (g_mock_timer_config[5].callback != NULL)
        g_mock_timer_config[5].callback();
    app_motion_timer_snapshot_t snapshot;
    app_motion_timer_get_snapshot(&snapshot);
    CHECK(snapshot.position.count == 1 && snapshot.velocity.count == 1);
    CHECK(snapshot.velocity.feedback_valid);
    extern void mock_advance_cycle(uint32_t cycles);
    mock_advance_cycle(96000U);
    if (g_mock_timer_config[5].callback != NULL)
        g_mock_timer_config[5].callback();
    app_motion_timer_get_snapshot(&snapshot);
    CHECK(snapshot.velocity.late_count == 1);
    app_motion_timer_take_window(&snapshot);
    CHECK(snapshot.velocity.window_count == 2);
    CHECK(snapshot.velocity.window_late_count == 1);
    mock_advance_cycle(48000U);
    if (g_mock_timer_config[5].callback)
        g_mock_timer_config[5].callback();
    app_motion_timer_take_window(&snapshot);
    CHECK(snapshot.velocity.count == 3 && snapshot.velocity.window_count == 1);
    CHECK(snapshot.velocity.period_min_cycles == 48000U);
    CHECK(snapshot.velocity.period_max_cycles == 48000U);
    CHECK(snapshot.velocity.window_late_count == 0);
    app_motion_timer_stop();
    CHECK(!g_mock_timer_running[4] && !g_mock_timer_running[5]);
    CHECK(app_motion_timer_start() == 0);
    if (g_mock_timer_config[5].callback != NULL)
        g_mock_timer_config[5].callback();
    app_motion_timer_get_snapshot(&snapshot);
    CHECK(snapshot.velocity.count == 1); /* 新启动窗口不沿用停止前时间 */
    app_motion_timer_stop();
    extern int g_mock_timer_start_failure;
    g_mock_timer_start_failure = 5;
    CHECK(app_motion_timer_start() != 0);
    CHECK(!g_mock_timer_running[4] && !g_mock_timer_running[5]);
    g_mock_timer_start_failure = -1;
}

static unsigned s_position_bind_calls;
static void position_bind_callback(void) { s_position_bind_calls++; }

/** @brief CH0 位置拍绑定回调（镜像 velocity 绑定） */
void test_motion_timer_position_bind(void) {
    s_position_bind_calls = 0;
    CHECK(app_motion_timer_init() == 0);
    app_motion_timer_bind_position(position_bind_callback);
    CHECK(app_motion_timer_start() == 0);
    if (g_mock_timer_config[4].callback != NULL)
        g_mock_timer_config[4].callback();
    CHECK(s_position_bind_calls == 1);
    app_motion_timer_snapshot_t snapshot;
    app_motion_timer_get_snapshot(&snapshot);
    CHECK(snapshot.position.count == 1);
    app_motion_timer_bind_position(NULL); /* 解绑后不再调用 */
    if (g_mock_timer_config[4].callback != NULL)
        g_mock_timer_config[4].callback();
    CHECK(s_position_bind_calls == 1);
    app_motion_timer_stop();
}
