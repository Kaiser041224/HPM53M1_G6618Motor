/**
 * @file    test_fault_policy.c
 * @brief   逐故障动作策略表（policy）+ 分级动作 + 台架注入 主机自测
 * @author  Kaiser
 *
 * 覆盖目标：
 *   ① 设计表（app_fault_policy_default）与设计一致（oc_fast=trip / oc_slow=derate /
 *      vbus=stop / adc_timeout=trip / enc_read=warn）；且编译期台架总保险成立：
 *      `software.fault.actions_enable=0`（台架默认）时**生效**表全部退为 WARN，
 *      且运行期可用 app_fault_set_actions_enable() 切换（取代原编译期宏）；
 *   ② `app_fault_get_action()` 取"当前生效的最高级动作"（TRIP > STOP > DERATE > WARN）；
 *   ③ WARN 码不进入 FAULT、不阻塞使能；带动作码（STOP/TRIP）进入 FAULT 且阻塞使能；
 *   ④ `inject` 可覆盖式注入与解除（台架验证动作路径用，不依赖物理阈值）；
 *   ⑤ `app_fault_get_limit_scale()` 仅在 DERATE 生效时 < 1.0。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_fault.h"
#include "test_util.h"

extern void mock_deps_reset(void);
extern void mock_params_defaults(void);
extern void mock_set_raw(adc_channel_t ch, uint16_t raw);
extern void mock_set_vbus_fast(float v);

/** @brief 清空注入 + 锁存，回到干净态 */
static void clear_all(void) {
    (void)app_fault_inject(APP_FAULT_OC_FAST_U, false);
    (void)app_fault_inject(APP_FAULT_OC_FAST_V, false);
    (void)app_fault_inject(APP_FAULT_OC_FAST_W, false);
    (void)app_fault_inject(APP_FAULT_OC_SLOW_U, false);
    (void)app_fault_inject(APP_FAULT_ENC_READ, false);
    (void)app_fault_clear();
}

static void setup(void) {
    mock_params_defaults();
    mock_deps_reset();
    /* 三相 raw 置于 WDOG 窗内：否则 fault_conditions_ok() 不成立 → clear 会被正确拒绝 */
    mock_set_raw(ADC_CH_I_U, 32768U);
    mock_set_raw(ADC_CH_I_V, 32768U);
    mock_set_raw(ADC_CH_I_W, 32768U);
    app_fault_init(NULL);
}

void test_fault_policy_defaults(void) {
    setup();

    /* ① 设计表（不随台架开关变化） */
    CHECK(app_fault_policy_default(APP_FAULT_OC_FAST_U) == APP_FLT_ACT_TRIP);
    CHECK(app_fault_policy_default(APP_FAULT_OC_SLOW_U) == APP_FLT_ACT_DERATE);
    CHECK(app_fault_policy_default(APP_FAULT_VBUS_OV) == APP_FLT_ACT_STOP);
    CHECK(app_fault_policy_default(APP_FAULT_VBUS_UV) == APP_FLT_ACT_STOP);
    CHECK(app_fault_policy_default(APP_FAULT_ADC_TIMEOUT) == APP_FLT_ACT_TRIP);
    CHECK(app_fault_policy_default(APP_FAULT_ENC_READ) == APP_FLT_ACT_WARN);
    CHECK(app_fault_policy_default(APP_FAULT_OC_TRIP) == APP_FLT_ACT_TRIP);
    /* 台架默认（actions_enable=0）：生效表全 WARN（行为与改动前逐字段一致） */
    CHECK(!app_fault_actions_enabled());
    CHECK(app_fault_get_policy(APP_FAULT_OC_FAST_U) == APP_FLT_ACT_WARN);
    CHECK(app_fault_get_policy(APP_FAULT_VBUS_OV) == APP_FLT_ACT_WARN);

    /* 无故障 → 最低级动作、无阻塞、限幅倍率 1.0 */
    CHECK(app_fault_get_action() == APP_FLT_ACT_WARN);
    CHECK(app_fault_get_blocking_latched() == 0U);
    CHECK_NEAR(app_fault_get_limit_scale(), 1.0f, 1e-6f);
}

void test_fault_policy_action_levels(void) {
    setup();
    clear_all();

    /* ②③ WARN 码：只计数，不进 FAULT、不阻塞使能 */
    app_fault_set_policy(APP_FAULT_ENC_READ, APP_FLT_ACT_WARN);
    (void)app_fault_inject(APP_FAULT_ENC_READ, true);
    CHECK((app_fault_get_codes() & APP_FAULT_ENC_READ) != 0U);
    CHECK(app_fault_get_action() == APP_FLT_ACT_WARN);
    CHECK(app_fault_get_state() == APP_FAULT_STATE_WARNING);
    CHECK(app_fault_get_blocking_latched() == 0U);
    (void)app_fault_inject(APP_FAULT_ENC_READ, false);

    /* ④ TRIP 码：进 FAULT、阻塞使能 */
    app_fault_set_policy(APP_FAULT_OC_FAST_U, APP_FLT_ACT_TRIP);
    (void)app_fault_inject(APP_FAULT_OC_FAST_U, true);
    CHECK(app_fault_get_action() == APP_FLT_ACT_TRIP);
    CHECK(app_fault_get_state() == APP_FAULT_STATE_FAULT);
    CHECK((app_fault_get_blocking_latched() & APP_FAULT_OC_FAST_U) != 0U);
    (void)app_fault_inject(APP_FAULT_OC_FAST_U, false);
    CHECK(app_fault_clear() == 0);

    /* ⑤ DERATE 码：不进 FAULT，但压低限幅倍率 */
    app_fault_set_policy(APP_FAULT_OC_SLOW_U, APP_FLT_ACT_DERATE);
    (void)app_fault_inject(APP_FAULT_OC_SLOW_U, true);
    CHECK(app_fault_get_action() == APP_FLT_ACT_DERATE);
    CHECK(app_fault_get_state() != APP_FAULT_STATE_FAULT);
    CHECK(app_fault_get_blocking_latched() == 0U);
    CHECK(app_fault_get_limit_scale() < 1.0f);
    (void)app_fault_inject(APP_FAULT_OC_SLOW_U, false);
    CHECK_NEAR(app_fault_get_limit_scale(), 1.0f, 1e-6f);

    /* ⑥ 取最高级：DERATE + TRIP 同时存在 → TRIP */
    app_fault_set_policy(APP_FAULT_OC_SLOW_U, APP_FLT_ACT_DERATE);
    app_fault_set_policy(APP_FAULT_OC_FAST_W, APP_FLT_ACT_TRIP);
    (void)app_fault_inject(APP_FAULT_OC_SLOW_U, true);
    (void)app_fault_inject(APP_FAULT_OC_FAST_W, true);
    CHECK(app_fault_get_action() == APP_FLT_ACT_TRIP);
    clear_all();
}

void test_fault_policy_runtime_override(void) {
    setup();
    clear_all();

    /* ⑦ 运行期改表（RAM）：enc_read 升为 trip → 立刻生效 */
    app_fault_set_policy(APP_FAULT_ENC_READ, APP_FLT_ACT_TRIP);
    CHECK(app_fault_get_policy(APP_FAULT_ENC_READ) == APP_FLT_ACT_TRIP);
    (void)app_fault_inject(APP_FAULT_ENC_READ, true);
    CHECK(app_fault_get_action() == APP_FLT_ACT_TRIP);
    CHECK((app_fault_get_blocking_latched() & APP_FAULT_ENC_READ) != 0U);

    (void)app_fault_inject(APP_FAULT_ENC_READ, false);
    CHECK(app_fault_clear() == 0);
    app_fault_set_policy(APP_FAULT_ENC_READ, APP_FLT_ACT_WARN);
    CHECK(app_fault_get_policy(APP_FAULT_ENC_READ) == APP_FLT_ACT_WARN);
}

void test_fault_derate_releases_with_condition(void) {
    setup();
    clear_all();
    app_fault_set_policy(APP_FAULT_OC_SLOW_U, APP_FLT_ACT_DERATE);

    /* 真实登记（进 latched + active）→ DERATE 生效 */
    app_fault_raise(APP_FAULT_OC_SLOW_U);
    CHECK(app_fault_get_limit_scale() < 1.0f);

    /* 条件消失（RMS 回 0，tick 里清 active；latched 仍保持）→ DERATE **自动解除** */
    for (int i = 0; i < 60; i++) {
        app_fault_tick();
    }
    CHECK((app_fault_get_latched() & APP_FAULT_OC_SLOW_U) != 0U); /* 仍锁存 */
    CHECK_NEAR(app_fault_get_limit_scale(), 1.0f, 1e-6f);         /* 但限幅已释放 */
}

void test_fault_actions_enable_switch(void) {
    setup();
    clear_all();

    /* 关闭（默认）：生效表全 WARN —— 只判断不动作 */
    CHECK(!app_fault_actions_enabled());
    CHECK(app_fault_get_policy(APP_FAULT_OC_FAST_U) == APP_FLT_ACT_WARN);

    /* 打开总开关 → 生效表 = 设计表（取代原编译期 software.fault.actions_enable） */
    app_fault_set_actions_enable(true);
    CHECK(app_fault_actions_enabled());
    CHECK(app_fault_get_policy(APP_FAULT_OC_FAST_U) == APP_FLT_ACT_TRIP);
    CHECK(app_fault_get_policy(APP_FAULT_VBUS_OV) == APP_FLT_ACT_STOP);
    CHECK(app_fault_get_policy(APP_FAULT_ENC_READ) == APP_FLT_ACT_WARN);

    /* 再关闭 → 回到全 WARN，且不残留动作 */
    app_fault_set_actions_enable(false);
    CHECK(app_fault_get_policy(APP_FAULT_OC_FAST_U) == APP_FLT_ACT_WARN);
    CHECK(app_fault_get_action() == APP_FLT_ACT_WARN);
}

/**
 * @brief 注入应留下首故障（诊断一致性）
 * 台架实测：fault inject 后 latched≠0 而 first=0，fault show 自相矛盾。
 */
void test_fault_inject_sets_first(void) {
    setup();
    clear_all();

    (void)app_fault_inject(APP_FAULT_VBUS_UV, true);
    CHECK(app_fault_get_first() == APP_FAULT_VBUS_UV);
    (void)app_fault_inject(APP_FAULT_VBUS_UV, false);
    CHECK(app_fault_clear() == 0);
}

/**
 * @brief STOP 类注入必须产生一次受控停机请求（边沿）
 * 台架缺口：此前 inject 只改状态，桥仍带电 → STOP 路径无法验证。
 */
void test_fault_inject_stop_requests_stop(void) {
    setup();
    clear_all();
    app_fault_set_policy(APP_FAULT_VBUS_OV, APP_FLT_ACT_STOP);
    CHECK(app_fault_take_stop_request() == 0U);

    (void)app_fault_inject(APP_FAULT_VBUS_OV, true);
    CHECK(app_fault_get_action() == APP_FLT_ACT_STOP);
    CHECK(app_fault_take_stop_request() == APP_FAULT_VBUS_OV);
    CHECK(app_fault_take_stop_request() == 0U); /* 边沿：取走即清 */

    /* TRIP 类不产生停机请求（走 state=FAULT 的 ISR 门控） */
    app_fault_set_policy(APP_FAULT_OC_FAST_U, APP_FLT_ACT_TRIP);
    (void)app_fault_inject(APP_FAULT_OC_FAST_U, true);
    CHECK(app_fault_take_stop_request() == 0U);
    (void)app_fault_inject(APP_FAULT_OC_FAST_U, false);
    (void)app_fault_inject(APP_FAULT_VBUS_OV, false);
    CHECK(app_fault_clear() == 0);
}
