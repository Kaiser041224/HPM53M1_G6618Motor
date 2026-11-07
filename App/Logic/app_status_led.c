/**
 * @file    app_status_led.c
 * @brief   状态 LED 灯语实现（io 任务内推进，不进入 ISR）
 * @author  Kaiser
 *
 * 波形全部由"模式起始时间 + 相位"计算，无动态分配、无阻塞延时；
 * 每次 app_status_led_step() 只做几次比较和一次 GPIO 写。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_status_led.h"

#include <stddef.h>

#include "app_can_id.h"
#include "app_fault.h"
#include "app_foc.h"
#include "app_gpio.h"
#include "intf_gpio.h"

/* ============================================================================
 * 灯语参数（毫秒）
 * ============================================================================ */
typedef enum {
    LED_MODE_IDLE = 0, /**< 上电未运行：2Hz 方波 */
    LED_MODE_RUN,      /**< READY/RUN：按节点 ID 的短脉冲 */
    LED_MODE_CALIB,    /**< 辨识中：1s 双闪 */
    LED_MODE_FAULT,    /**< 故障锁存：SOS */
} led_mode_t;

#define LED_IDLE_PERIOD_MS (500U)
#define LED_IDLE_ON_MS     (250U)

#define LED_RUN_PERIOD_BASE_MS (2000U)
#define LED_RUN_ON_MS          (80U)

#define LED_INVALID_PERIOD_MS  (2000U)
#define LED_INVALID_ON1_MS     (80U)
#define LED_INVALID_ON2_BEGIN  (280U)
#define LED_INVALID_ON2_END    (360U)

#define LED_CALIB_PERIOD_MS (1000U)
#define LED_CALIB_ON1_MS    (100U)
#define LED_CALIB_ON2_BEGIN (200U)
#define LED_CALIB_ON2_END   (300U)

#define LED_SOS_PERIOD_MS (4560U)

/** SOS：三短 / 三长 / 三短，区间为 [begin, end) */
static const uint16_t s_sos_on[][2] = {
    {0U, 120U},    {240U, 360U},    {480U, 600U},    /* S */
    {960U, 1320U}, {1440U, 1800U},  {1920U, 2280U},  /* O */
    {2640U, 2760U}, {2880U, 3000U}, {3120U, 3240U},  /* S */
};

static app_status_led_ctx_t s_ctx;

/* ============================================================================
 * 内部工具
 * ============================================================================ */

/** @brief 目标灯语模式（按优先级） */
static led_mode_t led_target_mode(const app_status_led_input_t* in) {
    if ((in == NULL) || in->fault_latched || (in->foc == APP_STATUS_LED_FOC_FAULT)) {
        return LED_MODE_FAULT;
    }
    if (in->foc == APP_STATUS_LED_FOC_CALIB) {
        return LED_MODE_CALIB;
    }
    if ((in->foc == APP_STATUS_LED_FOC_READY) || (in->foc == APP_STATUS_LED_FOC_RUN)) {
        return LED_MODE_RUN;
    }
    return LED_MODE_IDLE;
}

/** @brief RUN 模式相位判亮：ID 有效为单脉冲，无效为双脉冲 */
static bool led_run_on(const app_status_led_input_t* in, uint32_t phase) {
    if (!in->node_id_valid || (in->node_id >= 8U)) {
        uint32_t p = phase % LED_INVALID_PERIOD_MS;

        return (p < LED_INVALID_ON1_MS)
               || ((p >= LED_INVALID_ON2_BEGIN) && (p < LED_INVALID_ON2_END));
    }

    uint32_t divisor = (uint32_t) in->node_id + 1U;
    /* 四舍五入到最近毫秒，与规格表一致（如 ID2=667ms、ID6=286ms） */
    uint32_t period = (LED_RUN_PERIOD_BASE_MS + (divisor / 2U)) / divisor;

    return (phase % period) < LED_RUN_ON_MS;
}

/* ============================================================================
 * 公开接口
 * ============================================================================ */

void app_status_led_init(void) {
    const intf_gpio_cfg_t cfg = {
        .pin = PIN_LED_STATUS,
        .dir = INTF_GPIO_DIR_OUTPUT,
        .pull = INTF_GPIO_PULL_NONE,
        .init_level = INTF_GPIO_LEVEL_HIGH, /* 低电平点亮：高 = 灭 */
        .irq_mode = INTF_GPIO_IRQ_NONE,
        .irq_cb = NULL,
        .irq_user_data = NULL,
    };

    (void) intf_gpio_init(&cfg);
    app_status_led_ctx_init(&s_ctx, 0U);
}

void app_status_led_step(uint32_t now_ms) {
    app_status_led_input_t in;

    switch (app_foc_get_state()) {
    case APP_FOC_STATE_READY: in.foc = APP_STATUS_LED_FOC_READY; break;
    case APP_FOC_STATE_RUN: in.foc = APP_STATUS_LED_FOC_RUN; break;
    case APP_FOC_STATE_CALIB: in.foc = APP_STATUS_LED_FOC_CALIB; break;
    case APP_FOC_STATE_FAULT: in.foc = APP_STATUS_LED_FOC_FAULT; break;
    case APP_FOC_STATE_OFF:
    default: in.foc = APP_STATUS_LED_FOC_OFF; break;
    }

    in.fault_latched = (app_fault_get_state() == APP_FAULT_STATE_FAULT);
    in.node_id = app_can_id_get_boot_id();
    in.node_id_valid = (in.node_id < 8U);

    bool on = app_status_led_eval(&s_ctx, &in, now_ms);

    /* PB01 低电平点亮：0 = 亮，1 = 灭 */
    app_gpio_set(PIN_LED_STATUS, on ? 0U : 1U);
}

void app_status_led_ctx_init(app_status_led_ctx_t* ctx, uint32_t now_ms) {
    if (ctx == NULL) {
        return;
    }
    ctx->mode = (uint8_t) LED_MODE_IDLE;
    ctx->mode_start_ms = now_ms;
}

bool app_status_led_eval(app_status_led_ctx_t* ctx, const app_status_led_input_t* in,
                         uint32_t now_ms) {
    if ((ctx == NULL) || (in == NULL)) {
        return false;
    }

    led_mode_t mode = led_target_mode(in);

    /* 模式切换时相位从切换时刻重新起算，避免半截波形 */
    if (ctx->mode != (uint8_t) mode) {
        ctx->mode = (uint8_t) mode;
        ctx->mode_start_ms = now_ms;
    }

    uint32_t phase = now_ms - ctx->mode_start_ms;

    switch (mode) {
    case LED_MODE_IDLE:
        return (phase % LED_IDLE_PERIOD_MS) < LED_IDLE_ON_MS;

    case LED_MODE_RUN:
        return led_run_on(in, phase);

    case LED_MODE_CALIB: {
        uint32_t p = phase % LED_CALIB_PERIOD_MS;

        return (p < LED_CALIB_ON1_MS) || ((p >= LED_CALIB_ON2_BEGIN) && (p < LED_CALIB_ON2_END));
    }

    case LED_MODE_FAULT: {
        uint32_t p = phase % LED_SOS_PERIOD_MS;

        for (size_t i = 0U; i < (sizeof(s_sos_on) / sizeof(s_sos_on[0])); i++) {
            if ((p >= s_sos_on[i][0]) && (p < s_sos_on[i][1])) {
                return true;
            }
        }
        return false;
    }

    default:
        return false;
    }
}
