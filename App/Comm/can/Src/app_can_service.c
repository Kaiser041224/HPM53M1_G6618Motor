/**
 * @file    app_can_service.c
 * @brief   DM 兼容 CAN 运行时服务实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_can_service.h"

#include "app_analog_signal.h"
#include "app_can.h"
#include "app_can_policy.h"
#include "app_can_protocol.h"
#include "app_fault.h"
#include "app_foc.h"
#include "app_foc_current.h"
#include "app_motion.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "foc_math.h"
#include "intf_clock.h"

#include <stddef.h>
#include <string.h>

/** @brief 接收队列深度（2 的幂，位掩码取模） */
#define CAN_RX_QUEUE_SIZE (32U)
/** @brief 每拍最多处理的帧数（有界，防单帧风暴阻塞 1kHz 任务） */
#define CAN_RX_BUDGET_PER_STEP (8U)

/** @brief 队列项 */
typedef struct {
    uint32_t id;
    uint8_t len;
    uint8_t data[8];
} can_rx_item_t;

static can_rx_item_t s_rxq[CAN_RX_QUEUE_SIZE];
static volatile uint8_t s_rx_head;
static volatile uint8_t s_rx_tail;

static uint8_t s_node_id = 1U;
static uint16_t s_timeout_ms; /**< 0 = 禁用 */
static uint32_t s_cycles_per_ms;
static bool s_initialized;

static volatile uint32_t s_last_control_cycles; /**< ISR 写入：最后有效控制帧 */
static volatile bool s_wd_armed;                /**< 首个控制帧后置位 */
static bool s_wd_suspended;
static bool s_wd_fired;

static app_can_service_stats_t s_stats;

/* ── RX 入队 ───────────────────────────────────────────────────────── */

/**
 * @brief RX 中断钩子：只记录看门狗时间戳
 *
 * 必须在真 ISR 上下文完成：`foc off` 自旋 3 s 期间不执行 poll，
 * 否则会把"有意的停机静默"误判为通信丢失。
 */
static void can_rx_isr_hook(uint32_t id, const uint8_t* data, uint8_t len) {
    uint8_t type = 0U;
    uint8_t node = 0U;

    (void)data;
    (void)len;
    if (!app_can_dm_parse_id(id, &type, &node) || (node != s_node_id)) {
        return;
    }
    if (type <= APP_CAN_DM_TYPE_CLEAR) { /* 控制/使能/失能/清故障 */
        s_last_control_cycles = intf_clock_get_cycle();
        s_wd_armed = true;
        s_wd_fired = false;
    }
}

void app_can_service_feed_watchdog(void) {
    s_last_control_cycles = intf_clock_get_cycle();
    s_wd_armed = true;
    s_wd_fired = false;
}

/** @brief RX 回调（任务上下文）：只入队 */
static void can_rx_callback(const app_can_msg_t* msg) {
    if (msg == NULL) {
        return;
    }
    uint8_t next = (uint8_t)((s_rx_head + 1U) % CAN_RX_QUEUE_SIZE);

    if (next == s_rx_tail) {
        s_stats.rx_dropped++;
        return;
    }
    s_rxq[s_rx_head].id = msg->id;
    s_rxq[s_rx_head].len = (msg->dlc > 8U) ? 8U : msg->dlc;
    if (s_rxq[s_rx_head].len > 0U) {
        memcpy(s_rxq[s_rx_head].data, msg->data, s_rxq[s_rx_head].len);
    }
    s_rx_head = next;
}

/* ── 执行 ─────────────────────────────────────────────────────────── */

/**
 * @brief 反馈温度：℃ → uint8（无效 0xFF，越界钳 0..254）
 */
static uint8_t can_temp_u8(adc_channel_t ch) {
    float temp_c = 0.0f;

    if (!app_analog_signal_ntc_temp_c(ch, &temp_c)) {
        return APP_CAN_DM_TEMP_INVALID;
    }
    if (!(temp_c > 0.0f)) {
        return 0U;
    }
    if (temp_c >= 254.0f) {
        return 254U;
    }
    return (uint8_t)temp_c;
}

/**
 * @brief 组一帧反馈并发送（问询式：每条有效帧回一帧）
 */
static void can_send_feedback(void) {
    app_motion_snapshot_t snap;
    app_foc_current_snapshot_t current;
    const app_motor_params_t* motor = app_motor_params_current();
    app_foc_state_t state = app_foc_get_state();
    bool enabled = (state == APP_FOC_STATE_READY) || (state == APP_FOC_STATE_RUN);
    uint8_t data[8];
    float pos_rad = 0.0f;
    float vel_rad_s = 0.0f;
    float torque_nm = 0.0f;

    app_motion_get_snapshot(&snap);
    app_foc_current_get_snapshot(&current);
    bool valid = snap.feedback_valid && snap.calibrated;

    if (valid) {
        float dir = (snap.output_direction < 0.0f) ? -1.0f : 1.0f;

        pos_rad = foc_wrap_pm_pi(snap.shaft_rad * dir);
        vel_rad_s = snap.speed_rad_s * dir;
        if ((motor->kt_nm_per_a > 0.0f) && (motor->reducer_ratio > 0.0f)) {
            torque_nm = motor->kt_nm_per_a * current.i_q_a * motor->reducer_ratio * dir;
        }
    }

    uint8_t err = app_can_dm_err_from_fault(app_fault_get_latched(), valid, enabled);

    if (app_can_dm_encode_feedback(data, s_node_id, err, pos_rad, vel_rad_s, torque_nm,
                                   can_temp_u8(ADC_CH_NTC0), can_temp_u8(ADC_CH_NTC1),
                                   NULL)
        != 8U) {
        return;
    }
    if (app_can_send_std((uint16_t)app_can_dm_feedback_id(s_node_id), data, 8U) != 0) {
        s_stats.tx_fail++;
    }
}

/**
 * @brief 执行一个已规划动作；拒绝只计数，不置故障
 */
static void can_exec_action(app_can_dm_action_t action, const app_can_dm_frame_t* frame) {
    const app_motor_params_t* motor = app_motor_params_current();
    int rc = 0;

    switch (action) {
    case APP_CAN_DM_ACT_ENABLE:
        rc = app_foc_enable();
        break;
    case APP_CAN_DM_ACT_DISABLE:
        if (app_foc_get_state() == APP_FOC_STATE_OFF) {
            (void)app_motion_release_output();
        } else {
            rc = app_motion_stop();
        }
        break;
    case APP_CAN_DM_ACT_CLEAR:
        if (app_foc_get_state() != APP_FOC_STATE_OFF) {
            rc = -1; /* 清故障只允许在 OFF；FAULT 态需先失能 */
        } else {
            rc = app_fault_clear();
        }
        break;
    case APP_CAN_DM_ACT_MIT: {
        app_motion_snapshot_t snap;
        const app_software_params_t* sw = app_software_params_current();

        app_motion_get_snapshot(&snap);
        if (!snap.continuous_valid) {
            rc = APP_MOTION_CMD_NOT_CALIBRATED;
            break;
        }
        float dir = (snap.output_direction < 0.0f) ? -1.0f : 1.0f;
        float shaft_single_user = foc_wrap_pm_pi(snap.shaft_rad * dir);
        float shaft_cont_user = snap.shaft_continuous_rad * dir;
        float target = shaft_cont_user
                       + foc_wrap_pm_pi(frame->p_des_rad - shaft_single_user);
        float kp = (frame->kp > sw->mit.kp_max) ? sw->mit.kp_max : frame->kp;
        float kd = (frame->kd > sw->mit.kd_max) ? sw->mit.kd_max : frame->kd;

        if (kp < 0.0f) {
            kp = 0.0f;
        }
        if (kd < 0.0f) {
            kd = 0.0f;
        }
        rc = app_motion_mit(target, frame->v_des_rad_s, kp, kd, frame->t_ff_nm);
        break;
    }
    case APP_CAN_DM_ACT_POSITION: {
        float v_lim = (frame->v_des_rad_s < 0.0f) ? -frame->v_des_rad_s : frame->v_des_rad_s;

        if (!(v_lim >= 0.01f)) {
            v_lim = 0.01f;
        }
        if (v_lim > APP_CAN_DM_V_MAX_RAD_S) {
            v_lim = APP_CAN_DM_V_MAX_RAD_S;
        }
        rc = app_motion_position_limited(foc_wrap_2pi(frame->p_des_rad), v_lim, 0.0f, 0.0f);
        break;
    }
    case APP_CAN_DM_ACT_FORCE_POS: {
        float v_lim = frame->vel_limit_rad_s;
        float iq_limit = frame->iq_ratio * motor->i_peak_10s_a;

        if (!(v_lim >= 0.01f)) {
            v_lim = 0.01f;
        }
        if (v_lim > 100.0f) {
            v_lim = 100.0f;
        }
        rc = app_motion_position_limited(foc_wrap_2pi(frame->p_des_rad), v_lim, 0.0f, iq_limit);
        break;
    }
    case APP_CAN_DM_ACT_VELOCITY:
        rc = app_motion_speed(frame->v_des_rad_s);
        break;
    default:
        break;
    }

    if (rc != 0) {
        s_stats.rejected++;
    }
}

/* ── 队列处理 ─────────────────────────────────────────────────────── */

static void can_process_rx(void) {
    const app_software_params_t* sw = app_software_params_current();
    bool mit_enabled = (sw->mit.enable != 0U);

    for (uint8_t n = 0U; (n < CAN_RX_BUDGET_PER_STEP) && (s_rx_tail != s_rx_head); n++) {
        can_rx_item_t item = s_rxq[s_rx_tail];

        s_rx_tail = (uint8_t)((s_rx_tail + 1U) % CAN_RX_QUEUE_SIZE);

        app_can_dm_frame_t frame;
        int rc = app_can_dm_decode(item.id, item.data, item.len, s_node_id, NULL, &frame);

        if (rc < 0) {
            s_stats.bad_frames++;
            continue;
        }
        if (rc > 0) {
            s_stats.foreign++;
            continue;
        }
        s_stats.rx_frames++;

        /* 快照可能在上一帧执行后变化；每帧重新规划。 */
        app_motion_snapshot_t snap;
        app_can_dm_action_t action;

        app_motion_get_snapshot(&snap);
        uint8_t active_mode = snap.active ? (uint8_t)snap.mode : APP_CAN_DM_MOTION_NONE;
        uint8_t foc_state = (uint8_t)app_foc_get_state();

        if (app_can_dm_plan(&frame, active_mode, snap.active, foc_state, mit_enabled, &action) != 0) {
            action = APP_CAN_DM_ACT_DROP;
        }

        if (action == APP_CAN_DM_ACT_RELEASE_OUTPUT) {
            (void)app_motion_release_output();
            s_stats.mode_switch++;
        } else if (action != APP_CAN_DM_ACT_DROP) {
            can_exec_action(action, &frame);
        } else {
            s_stats.rejected++; /* 策略丢弃：未使能 / MIT 关闭 / 未知状态 */
        }

        can_send_feedback();
    }
}

/** @brief 看门狗检查 */
static void can_watchdog_step(void) {
    if (s_wd_suspended || !s_wd_armed || (s_timeout_ms == 0U)) {
        return;
    }
    uint32_t timeout_cycles = (uint32_t)s_timeout_ms * s_cycles_per_ms;
    uint32_t elapsed = (uint32_t)(intf_clock_get_cycle() - s_last_control_cycles);

    if ((elapsed <= timeout_cycles) || s_wd_fired) {
        return;
    }
    s_wd_fired = true;
    app_fault_raise(APP_FAULT_COMM_TIMEOUT);
    if (app_fault_actions_enabled()
        && (app_fault_get_policy(APP_FAULT_COMM_TIMEOUT) == APP_FLT_ACT_STOP)) {
        (void)app_foc_request_fault_stop(APP_FAULT_COMM_TIMEOUT);
    }
}

/* ── 生命周期 ─────────────────────────────────────────────────────── */

void app_can_service_init(uint8_t node_id) {
    const app_software_params_t* sw = app_software_params_current();

    s_node_id = node_id;
    if (s_node_id > APP_CAN_DM_NODE_MAX) {
        s_node_id = 1U;
    }
    s_timeout_ms = sw->comm.comm_timeout_ms;
    s_cycles_per_ms = intf_clock_get_cpu_freq() / 1000U;
    s_rx_head = 0U;
    s_rx_tail = 0U;
    s_wd_armed = false;
    s_wd_suspended = false;
    s_wd_fired = false;
    memset(&s_stats, 0, sizeof(s_stats));
    s_last_control_cycles = intf_clock_get_cycle();

    app_can_set_rx_callback(can_rx_callback);
    app_can_set_rx_isr_hook(can_rx_isr_hook);
    /* 丢弃 service 注册前积压的报文：启动早期无回调，驱动环里的陈旧帧不应在
     * 运行期被当作命令回放（spec：早到帧无人接收属预期）。 */
    {
        app_can_msg_t stale;

        while (app_can_receive(&stale) == 0) {
        }
    }
    /* 接受全部标准帧（节点过滤在软件层做，避免过滤器把调试帧一起吞掉） */
    (void)app_can_add_std_filter(0x000U, 0x000U);
    s_initialized = true;
}

void app_can_service_step(void) {
    if (!s_initialized) {
        return;
    }
    app_can_poll();
    can_process_rx();
    can_watchdog_step();
}

void app_can_service_suspend_watchdog(bool suspend) { s_wd_suspended = suspend; }

bool app_can_service_watchdog_armed(void) { return s_wd_armed; }

void app_can_service_get_watchdog(bool* armed, bool* suspended, bool* fired) {
    if (armed != NULL) {
        *armed = s_wd_armed;
    }
    if (suspended != NULL) {
        *suspended = s_wd_suspended;
    }
    if (fired != NULL) {
        *fired = s_wd_fired;
    }
}

void app_can_service_flush_rx(void) { s_rx_tail = s_rx_head; }

uint8_t app_can_service_get_node_id(void) { return s_node_id; }

uint32_t app_can_service_get_dropped(void) { return s_stats.rx_dropped; }

void app_can_service_get_stats(app_can_service_stats_t* out) {
    if (out != NULL) {
        *out = s_stats;
    }
}
