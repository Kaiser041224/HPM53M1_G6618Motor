/**
 * @file    app_can_policy.c
 * @brief   DM CAN 帧 → 动作规划实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_can_policy.h"

#include "app_fault.h"

#include <stddef.h>

int app_can_dm_plan(const app_can_dm_frame_t* frame, uint8_t active_mode, bool active,
                    uint8_t foc_state, bool mit_enabled, app_can_dm_action_t* out) {
    uint8_t desired_mode;

    if ((frame == NULL) || (out == NULL)) {
        return -1;
    }
    *out = APP_CAN_DM_ACT_DROP;

    switch (frame->kind) {
    case APP_CAN_DM_KIND_ENABLE:
        *out = APP_CAN_DM_ACT_ENABLE;
        return 0;
    case APP_CAN_DM_KIND_DISABLE:
        *out = APP_CAN_DM_ACT_DISABLE;
        return 0;
    case APP_CAN_DM_KIND_CLEAR:
        *out = APP_CAN_DM_ACT_CLEAR;
        return 0;
    case APP_CAN_DM_KIND_MIT:
        if (!mit_enabled) {
            return 0;
        }
        desired_mode = APP_CAN_DM_MOTION_MIT;
        break;
    case APP_CAN_DM_KIND_POS_VEL:
    case APP_CAN_DM_KIND_FORCE_POS:
        desired_mode = APP_CAN_DM_MOTION_POSITION;
        break;
    case APP_CAN_DM_KIND_VEL:
        desired_mode = APP_CAN_DM_MOTION_SPEED;
        break;
    default:
        return 0;
    }

    if ((foc_state != APP_CAN_DM_FOC_READY) && (foc_state != APP_CAN_DM_FOC_RUN)) {
        return 0; /* 未使能：丢帧，主机按反馈重试 */
    }
    if (active && (active_mode != desired_mode)) {
        *out = APP_CAN_DM_ACT_RELEASE_OUTPUT;
        return 0;
    }

    switch (frame->kind) {
    case APP_CAN_DM_KIND_MIT:
        *out = APP_CAN_DM_ACT_MIT;
        break;
    case APP_CAN_DM_KIND_VEL:
        *out = APP_CAN_DM_ACT_VELOCITY;
        break;
    case APP_CAN_DM_KIND_FORCE_POS:
        *out = APP_CAN_DM_ACT_FORCE_POS;
        break;
    default:
        *out = APP_CAN_DM_ACT_POSITION;
        break;
    }
    return 0;
}

uint8_t app_can_dm_err_from_fault(uint32_t latched, bool feedback_valid, bool enabled) {
    const uint32_t oc_mask = APP_FAULT_OC_FAST_U | APP_FAULT_OC_FAST_V | APP_FAULT_OC_FAST_W
                             | APP_FAULT_OC_SLOW_U | APP_FAULT_OC_SLOW_V | APP_FAULT_OC_SLOW_W
                             | APP_FAULT_OC_TRIP;

    if ((latched & oc_mask) != 0U) {
        return 0xAU;
    }
    if ((latched & APP_FAULT_VBUS_OV) != 0U) {
        return 0x8U;
    }
    if ((latched & APP_FAULT_VBUS_UV) != 0U) {
        return 0x9U;
    }
    if ((latched & APP_FAULT_COMM_TIMEOUT) != 0U) {
        return 0xDU;
    }
    if (!feedback_valid || (latched != 0U)) {
        return 0xFU;
    }
    return enabled ? 0x1U : 0x0U;
}
