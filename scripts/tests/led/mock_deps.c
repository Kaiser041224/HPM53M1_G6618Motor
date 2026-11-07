/**
 * @file    mock_deps.c
 * @brief   app_status_led 主机自测的依赖替身（FOC/fault/CAN ID/GPIO）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_can_id.h"
#include "app_fault.h"
#include "app_foc.h"
#include "intf_gpio.h"

#include <string.h>

static app_foc_state_t s_foc_state;
static app_fault_state_t s_fault_state;
static uint8_t s_node_id;

int g_mock_led_gpio_writes;
int g_mock_led_last_pin;
int g_mock_led_last_level;
int g_mock_led_gpio_init_calls;
intf_gpio_cfg_t g_mock_led_gpio_cfg;

void mock_led_reset(void) {
    s_foc_state = APP_FOC_STATE_OFF;
    s_fault_state = APP_FAULT_STATE_NORMAL;
    s_node_id = 0U;
    g_mock_led_gpio_writes = 0;
    g_mock_led_last_pin = -1;
    g_mock_led_last_level = -1;
    g_mock_led_gpio_init_calls = 0;
    memset(&g_mock_led_gpio_cfg, 0, sizeof(g_mock_led_gpio_cfg));
}

void mock_led_set_foc(app_foc_state_t state) { s_foc_state = state; }

void mock_led_set_fault(app_fault_state_t state) { s_fault_state = state; }

void mock_led_set_node(uint8_t id) { s_node_id = id; }

app_foc_state_t app_foc_get_state(void) { return s_foc_state; }

app_fault_state_t app_fault_get_state(void) { return s_fault_state; }

uint8_t app_can_id_get_boot_id(void) { return s_node_id; }

void app_gpio_set(uint16_t pin, uint8_t on) {
    g_mock_led_gpio_writes++;
    g_mock_led_last_pin = (int) pin;
    g_mock_led_last_level = (int) on;
}

int intf_gpio_init(const intf_gpio_cfg_t* cfg) {
    g_mock_led_gpio_init_calls++;
    if (cfg != NULL) {
        g_mock_led_gpio_cfg = *cfg;
    }
    return 0;
}
