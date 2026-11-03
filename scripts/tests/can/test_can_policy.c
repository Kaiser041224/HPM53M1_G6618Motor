/**
 * @file    test_can_policy.c
 * @brief   DM 帧 → 动作规划（模式切换 / 使能门控 / MIT 开关）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_can_policy.h"
#include "app_fault.h"
#include "test_util.h"

#include <string.h>

static app_can_dm_frame_t frame_of(app_can_dm_kind_t kind) {
    app_can_dm_frame_t f;

    memset(&f, 0, sizeof(f));
    f.kind = kind;
    return f;
}

void test_can_dm_policy(void) {
    app_can_dm_frame_t f;
    app_can_dm_action_t a = APP_CAN_DM_ACT_DROP;

    /* 使能/失能/清故障：总是交执行层（前置条件由 FOC/故障模块判定） */
    f = frame_of(APP_CAN_DM_KIND_ENABLE);
    CHECK(app_can_dm_plan(&f, APP_CAN_DM_MOTION_NONE, false, APP_CAN_DM_FOC_OFF, false, &a) == 0);
    CHECK(a == APP_CAN_DM_ACT_ENABLE);

    f = frame_of(APP_CAN_DM_KIND_DISABLE);
    CHECK(app_can_dm_plan(&f, APP_CAN_DM_MOTION_POSITION, true, APP_CAN_DM_FOC_RUN, false, &a) == 0);
    CHECK(a == APP_CAN_DM_ACT_DISABLE);

    f = frame_of(APP_CAN_DM_KIND_CLEAR);
    CHECK(app_can_dm_plan(&f, APP_CAN_DM_MOTION_NONE, false, APP_CAN_DM_FOC_OFF, false, &a) == 0);
    CHECK(a == APP_CAN_DM_ACT_CLEAR);

    /* MIT 未启用 → 丢弃 */
    f = frame_of(APP_CAN_DM_KIND_MIT);
    CHECK(app_can_dm_plan(&f, APP_CAN_DM_MOTION_NONE, false, APP_CAN_DM_FOC_READY, false, &a) == 0);
    CHECK(a == APP_CAN_DM_ACT_DROP);

    /* 未使能 → 控制帧一律丢弃 */
    CHECK(app_can_dm_plan(&f, APP_CAN_DM_MOTION_NONE, false, APP_CAN_DM_FOC_OFF, true, &a) == 0);
    CHECK(a == APP_CAN_DM_ACT_DROP);
    CHECK(app_can_dm_plan(&f, APP_CAN_DM_MOTION_NONE, false, APP_CAN_DM_FOC_CALIB, true, &a) == 0);
    CHECK(a == APP_CAN_DM_ACT_DROP);

    /* 空闲态 + READY：直接进入对应模式 */
    CHECK(app_can_dm_plan(&f, APP_CAN_DM_MOTION_NONE, false, APP_CAN_DM_FOC_READY, true, &a) == 0);
    CHECK(a == APP_CAN_DM_ACT_MIT);

    f = frame_of(APP_CAN_DM_KIND_POS_VEL);
    CHECK(app_can_dm_plan(&f, APP_CAN_DM_MOTION_NONE, false, APP_CAN_DM_FOC_RUN, false, &a) == 0);
    CHECK(a == APP_CAN_DM_ACT_POSITION);

    f = frame_of(APP_CAN_DM_KIND_FORCE_POS);
    CHECK(app_can_dm_plan(&f, APP_CAN_DM_MOTION_NONE, false, APP_CAN_DM_FOC_RUN, false, &a) == 0);
    CHECK(a == APP_CAN_DM_ACT_FORCE_POS);

    f = frame_of(APP_CAN_DM_KIND_VEL);
    CHECK(app_can_dm_plan(&f, APP_CAN_DM_MOTION_NONE, false, APP_CAN_DM_FOC_RUN, false, &a) == 0);
    CHECK(a == APP_CAN_DM_ACT_VELOCITY);

    /* 同模式连续帧：不释放 */
    f = frame_of(APP_CAN_DM_KIND_POS_VEL);
    CHECK(app_can_dm_plan(&f, APP_CAN_DM_MOTION_POSITION, true, APP_CAN_DM_FOC_RUN, false, &a) == 0);
    CHECK(a == APP_CAN_DM_ACT_POSITION);

    /* POS_VEL 与 FORCE_POS 同属位置模式：不释放 */
    f = frame_of(APP_CAN_DM_KIND_FORCE_POS);
    CHECK(app_can_dm_plan(&f, APP_CAN_DM_MOTION_POSITION, true, APP_CAN_DM_FOC_RUN, false, &a) == 0);
    CHECK(a == APP_CAN_DM_ACT_FORCE_POS);

    /* 跨模式：释放输出 + 丢弃本帧 */
    f = frame_of(APP_CAN_DM_KIND_MIT);
    CHECK(app_can_dm_plan(&f, APP_CAN_DM_MOTION_POSITION, true, APP_CAN_DM_FOC_RUN, true, &a) == 0);
    CHECK(a == APP_CAN_DM_ACT_RELEASE_OUTPUT);

    f = frame_of(APP_CAN_DM_KIND_VEL);
    CHECK(app_can_dm_plan(&f, APP_CAN_DM_MOTION_POSITION, true, APP_CAN_DM_FOC_RUN, false, &a) == 0);
    CHECK(a == APP_CAN_DM_ACT_RELEASE_OUTPUT);

    f = frame_of(APP_CAN_DM_KIND_POS_VEL);
    CHECK(app_can_dm_plan(&f, APP_CAN_DM_MOTION_SPEED, true, APP_CAN_DM_FOC_RUN, false, &a) == 0);
    CHECK(a == APP_CAN_DM_ACT_RELEASE_OUTPUT);

    /* 参数非法 */
    CHECK(app_can_dm_plan(NULL, APP_CAN_DM_MOTION_NONE, false, APP_CAN_DM_FOC_OFF, false, &a) == -1);
    CHECK(app_can_dm_plan(&f, APP_CAN_DM_MOTION_NONE, false, APP_CAN_DM_FOC_OFF, false, NULL) == -1);
}

void test_can_dm_err_map(void) {
    CHECK(app_can_dm_err_from_fault(APP_FAULT_OC_FAST_U | APP_FAULT_VBUS_OV, true, true) == 0xAU);
    CHECK(app_can_dm_err_from_fault(APP_FAULT_OC_SLOW_W | APP_FAULT_VBUS_UV, true, true) == 0xAU);
    CHECK(app_can_dm_err_from_fault(APP_FAULT_VBUS_OV | APP_FAULT_COMM_TIMEOUT, true, true) == 0x8U);
    CHECK(app_can_dm_err_from_fault(APP_FAULT_VBUS_UV, true, true) == 0x9U);
    CHECK(app_can_dm_err_from_fault(APP_FAULT_COMM_TIMEOUT, true, true) == 0xDU);
    CHECK(app_can_dm_err_from_fault(APP_FAULT_ENC_READ, true, true) == 0xFU);
    CHECK(app_can_dm_err_from_fault(0U, false, true) == 0xFU);
    CHECK(app_can_dm_err_from_fault(0U, true, true) == 0x1U);
    CHECK(app_can_dm_err_from_fault(0U, true, false) == 0x0U);
}
