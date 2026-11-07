/**
 * @file    test_status_led.c
 * @brief   状态 LED 灯语波形与状态映射 主机自测
 * @author  Kaiser
 *
 * 覆盖：
 *   ① IDLE：2Hz 方波（亮 250 / 灭 250）
 *   ② RUN：ID 0..7 周期表（亮 80ms 脉冲），ID 无效为双脉冲
 *   ③ CALIB：1s 双闪
 *   ④ FAULT：SOS 时序（短-短-短 / 长-长-长 / 短-短-短）
 *   ⑤ 优先级与模式切换相位重置
 *   ⑥ 32 位毫秒时基回绕
 *   ⑦ 包装层：状态映射、GPIO 极性、初始化配置
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_status_led.h"

#include "app_can_id.h"
#include "app_fault.h"
#include "app_foc.h"
#include "app_gpio.h"
#include "intf_gpio.h"
#include "test_util.h"

int g_checks = 0;
int g_fails = 0;

extern void mock_led_reset(void);
extern void mock_led_set_foc(app_foc_state_t state);
extern void mock_led_set_fault(app_fault_state_t state);
extern void mock_led_set_node(uint8_t id);
extern int g_mock_led_gpio_writes;
extern int g_mock_led_last_pin;
extern int g_mock_led_last_level;
extern int g_mock_led_gpio_init_calls;
extern intf_gpio_cfg_t g_mock_led_gpio_cfg;

static app_status_led_ctx_t s_ctx;
static app_status_led_input_t s_in;

/** @brief 建立测试上下文并在 t0 落定模式 */
static void begin(app_status_led_foc_t foc, bool fault, uint8_t id, bool id_valid,
                  uint32_t t0) {
    s_in.foc = foc;
    s_in.fault_latched = fault;
    s_in.node_id = id;
    s_in.node_id_valid = id_valid;
    app_status_led_ctx_init(&s_ctx, t0);
    (void) app_status_led_eval(&s_ctx, &s_in, t0);
}

/** @brief 求 t 时刻的亮灭 */
static bool at(uint32_t t) { return app_status_led_eval(&s_ctx, &s_in, t); }

/* ============================================================================
 * IDLE：2Hz 方波
 * ============================================================================ */
static void test_idle_square_wave(void) {
    begin(APP_STATUS_LED_FOC_OFF, false, 0U, true, 0U);

    CHECK(at(0U));
    CHECK(at(249U));
    CHECK(!at(250U));
    CHECK(!at(499U));
    CHECK(at(500U));
    CHECK(at(749U));
    CHECK(!at(750U));
}

/* ============================================================================
 * RUN：ID 0..7 周期 = 2000/(ID+1)，亮 80ms
 * ============================================================================ */
static void test_run_id_periods(void) {
    static const uint16_t periods[8] = {2000U, 1000U, 667U, 500U, 400U, 333U, 286U, 250U};

    for (uint8_t id = 0U; id < 8U; id++) {
        begin(APP_STATUS_LED_FOC_RUN, false, id, true, 0U);
        CHECK(at(0U));
        CHECK(at(79U));
        CHECK(!at(80U));
        CHECK(!at((uint32_t) periods[id] - 1U));
        CHECK(at((uint32_t) periods[id]));
    }
}

static void test_run_invalid_id_double_pulse(void) {
    begin(APP_STATUS_LED_FOC_RUN, false, APP_CAN_ID_INVALID, false, 0U);

    CHECK(at(0U));
    CHECK(at(79U));
    CHECK(!at(80U));
    CHECK(!at(279U));
    CHECK(at(280U));
    CHECK(at(359U));
    CHECK(!at(360U));
    CHECK(!at(1999U));
    CHECK(at(2000U));
}

/* ============================================================================
 * CALIB：1s 双闪（100/100/100/700）
 * ============================================================================ */
static void test_calib_double_flash(void) {
    begin(APP_STATUS_LED_FOC_CALIB, false, 0U, true, 0U);

    CHECK(at(0U));
    CHECK(at(99U));
    CHECK(!at(100U));
    CHECK(!at(199U));
    CHECK(at(200U));
    CHECK(at(299U));
    CHECK(!at(300U));
    CHECK(!at(999U));
    CHECK(at(1000U));
}

/* ============================================================================
 * FAULT：SOS（周期 4560ms）
 * ============================================================================ */
static void test_fault_sos(void) {
    begin(APP_STATUS_LED_FOC_FAULT, false, 0U, true, 0U);

    /* S：3 短 */
    CHECK(at(0U));
    CHECK(at(119U));
    CHECK(!at(120U));
    CHECK(!at(239U));
    CHECK(at(240U));
    CHECK(at(359U));
    CHECK(!at(360U));
    CHECK(!at(479U));
    CHECK(at(480U));
    CHECK(at(599U));
    CHECK(!at(600U));
    CHECK(!at(959U));
    /* O：3 长 */
    CHECK(at(960U));
    CHECK(at(1319U));
    CHECK(!at(1320U));
    CHECK(!at(1439U));
    CHECK(at(1440U));
    CHECK(at(1799U));
    CHECK(!at(1800U));
    CHECK(!at(1919U));
    CHECK(at(1920U));
    CHECK(at(2279U));
    CHECK(!at(2280U));
    CHECK(!at(2639U));
    /* S：3 短 */
    CHECK(at(2640U));
    CHECK(at(2759U));
    CHECK(!at(2760U));
    CHECK(!at(2879U));
    CHECK(at(2880U));
    CHECK(at(2999U));
    CHECK(!at(3000U));
    CHECK(!at(3119U));
    CHECK(at(3120U));
    CHECK(at(3239U));
    CHECK(!at(3240U));
    CHECK(!at(4559U));
    /* 下一周期 */
    CHECK(at(4560U));
}

/* ============================================================================
 * 优先级：FAULT > CALIB > RUN > IDLE
 * ============================================================================ */
static void test_fault_overrides_calib_and_run(void) {
    begin(APP_STATUS_LED_FOC_CALIB, true, 5U, true, 0U);
    CHECK(at(0U));
    CHECK(!at(120U)); /* SOS 相位，不是双闪 */
    CHECK(!at(239U));
    CHECK(at(240U));

    begin(APP_STATUS_LED_FOC_RUN, true, 5U, true, 0U);
    CHECK(at(0U));
    CHECK(!at(120U));
    CHECK(!at(239U));
    CHECK(at(240U));
}

/* ============================================================================
 * 模式切换：相位从切换时刻重新起算
 * ============================================================================ */
static void test_mode_switch_resets_phase(void) {
    begin(APP_STATUS_LED_FOC_OFF, false, 0U, true, 0U);

    CHECK(at(200U)); /* IDLE 亮段 */

    s_in.foc = APP_STATUS_LED_FOC_RUN;
    s_in.node_id = 0U;
    s_in.node_id_valid = true;

    CHECK(at(300U));  /* 切到 RUN：phase 0 → 亮 */
    CHECK(at(379U));  /* phase 79 → 亮 */
    CHECK(!at(380U)); /* phase 80 → 灭 */
    CHECK(!at(2299U)); /* phase 1999 → 灭 */
    CHECK(at(2300U)); /* phase 2000 → 下一周期亮 */
}

/* ============================================================================
 * 32 位毫秒时基回绕
 * ============================================================================ */
static void test_time_wrap(void) {
    const uint32_t base = 0xFFFFFFF0U;

    begin(APP_STATUS_LED_FOC_OFF, false, 0U, true, base);
    CHECK(at(base));              /* phase 0 */
    CHECK(at(base + 249U));       /* phase 249（跨回绕） */
    CHECK(!at(base + 250U));      /* phase 250 */
    CHECK(at(base + 500U));       /* phase 500 → 下一周期 */
}

/* ============================================================================
 * 包装层：状态映射 + GPIO 极性 + 初始化
 * ============================================================================ */
static void test_wrapper_gpio_polarity(void) {
    mock_led_reset();
    mock_led_set_foc(APP_FOC_STATE_OFF);
    mock_led_set_fault(APP_FAULT_STATE_NORMAL);
    mock_led_set_node(3U);

    app_status_led_init();
    CHECK(g_mock_led_gpio_init_calls == 1);
    CHECK(g_mock_led_gpio_cfg.pin == PIN_LED_STATUS);
    CHECK(g_mock_led_gpio_cfg.dir == INTF_GPIO_DIR_OUTPUT);
    CHECK(g_mock_led_gpio_cfg.init_level == INTF_GPIO_LEVEL_HIGH); /* 初始灭 */

    app_status_led_step(0U);
    CHECK(g_mock_led_gpio_writes == 1);
    CHECK(g_mock_led_last_pin == (int) PIN_LED_STATUS);
    CHECK(g_mock_led_last_level == 0); /* IDLE phase 0：亮 = 低电平 */

    app_status_led_step(250U);
    CHECK(g_mock_led_last_level == 1); /* 灭 = 高电平 */
}

static void test_wrapper_maps_fault_state(void) {
    mock_led_reset();
    mock_led_set_foc(APP_FOC_STATE_RUN);
    mock_led_set_fault(APP_FAULT_STATE_FAULT);
    mock_led_set_node(0U);

    app_status_led_init();
    app_status_led_step(0U);
    CHECK(g_mock_led_last_level == 0); /* SOS phase 0：亮 */

    app_status_led_step(120U);
    CHECK(g_mock_led_last_level == 1); /* SOS 短闪间隙：灭 */
}

/* ============================================================================
 * 入口
 * ============================================================================ */
void test_idle_square_wave(void);
void test_run_id_periods(void);
void test_run_invalid_id_double_pulse(void);
void test_calib_double_flash(void);
void test_fault_sos(void);
void test_fault_overrides_calib_and_run(void);
void test_mode_switch_resets_phase(void);
void test_time_wrap(void);
void test_wrapper_gpio_polarity(void);
void test_wrapper_maps_fault_state(void);

int main(void) {
    test_idle_square_wave();
    test_run_id_periods();
    test_run_invalid_id_double_pulse();
    test_calib_double_flash();
    test_fault_sos();
    test_fault_overrides_calib_and_run();
    test_mode_switch_resets_phase();
    test_time_wrap();
    test_wrapper_gpio_polarity();
    test_wrapper_maps_fault_state();

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return (g_fails == 0) ? 0 : 1;
}
