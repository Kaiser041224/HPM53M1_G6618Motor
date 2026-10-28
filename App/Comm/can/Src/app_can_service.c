/**
 * @file    app_can_service.c
 * @brief   CAN 控制服务实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_can_service.h"

#include "app_can.h"
#include "app_can_protocol.h"
#include "app_fault.h"
#include "app_foc.h"
#include "app_motion.h"
#include "app_software_params.h"
#include "intf_clock.h"

#include <string.h>

/** @brief 接收队列深度（2 的幂，位掩码取模） */
#define CAN_RX_QUEUE_SIZE (32U)
/** @brief 每拍最多处理的帧数（有界，防单帧风暴阻塞 1kHz 任务） */
#define CAN_RX_BUDGET_PER_STEP (8U)
/** @brief Heartbeat 周期 [拍]（1kHz 任务） */
#define CAN_HEARTBEAT_TICKS (100U)
/** @brief encoder 回报周期 [拍]（用户定 1kHz） */
#define CAN_ENCODER_TICKS (1U)
/** @brief MIT 双帧配对窗口 [ms] */
#define CAN_MIT_PAIR_TIMEOUT_MS (2U)

#define CAN_DEG_PER_RAD (57.2957795f)
#define CAN_RAD_PER_DEG (0.01745329252f)
#define CAN_RAD_PER_RPM (0.10471975512f) /* 2π/60：出轴 rpm → rad/s */

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
static uint8_t s_mit_format;    /**< 0 = compact，1 = dual（init 期消费） */
static uint16_t s_timeout_ms;   /**< 0 = 禁用 */
static uint32_t s_cycles_per_ms;
static bool s_initialized;

static volatile uint32_t s_last_control_cycles; /**< ISR 写入：最后有效控制帧 */
static volatile bool s_wd_armed;                /**< 首个控制帧后置位 */
static bool s_wd_suspended;
static bool s_wd_fired;

static uint32_t s_tick;
static uint8_t s_control_mode;
static uint8_t s_input_mode;
static volatile bool s_estop_pending;

/** @brief MIT 双帧 A 暂存 */
static struct {
    bool valid;
    uint8_t seq;
    float p_deg;
    float v_rpm;
    uint32_t cycles;
} s_mit_a;

static app_can_service_stats_t s_stats;
/** 最近一条控制命令的拒绝码（0=OK；1..11 = -APP_MOTION_CMD_*），经 Heartbeat 回报 */
static uint8_t s_last_result;

/** @brief motion 返回码 → Heartbeat proc_result（0=OK，正数=拒绝码） */
static uint8_t can_result_code(int rc) { return (rc == 0) ? 0U : (uint8_t)(-rc); }

/** @brief 单位换算：rpm（出轴）→ rad/s（内部物理） */
static float can_rpm_to_rad_s(float rpm) { return rpm * CAN_RAD_PER_RPM; }

/** @brief 该命令是否算"喂狗"（只认被接受的控制帧） */
static bool can_cmd_feeds_watchdog(uint8_t cmd) {
    switch (cmd) {
    case APP_CAN_CMD_SET_AXIS_STATE:
    case APP_CAN_CMD_SET_CONTROLLER_MODE:
    case APP_CAN_CMD_SET_INPUT_POS:
    case APP_CAN_CMD_SET_INPUT_VEL:
    case APP_CAN_CMD_SET_INPUT_TORQUE:
    case APP_CAN_CMD_SET_LIMITS:
    case APP_CAN_CMD_SET_INPUT_MIT:
    case APP_CAN_CMD_SET_INPUT_MIT_A:
    case APP_CAN_CMD_SET_INPUT_MIT_B:
    case APP_CAN_CMD_CLEAR_ERRORS:
        return true;
    default:
        return false;
    }
}

/**
 * @brief RX 中断钩子：只记录看门狗时间戳
 *
 * 关键：它必须在真 ISR 上下文完成，否则 `foc off` 自旋 3s 期间不执行 poll，
 * 看门狗会把"有意的停机静默"误判为通信丢失。
 */
static void can_rx_isr_hook(uint32_t id, const uint8_t* data, uint8_t len) {
    uint8_t node = 0U;
    uint8_t cmd = 0U;

    (void)data;
    (void)len;
    if (!app_can_parse_id(id, &node, &cmd) || (node != s_node_id)) {
        return;
    }
    if (can_cmd_feeds_watchdog(cmd)) {
        s_last_control_cycles = intf_clock_get_cycle();
        s_wd_armed = true;
        s_wd_fired = false;
    }
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

/** @brief 发送标准帧（失败计数由 app_can 统计） */
static void can_send(uint8_t cmd, const uint8_t* data, uint8_t len) {
    (void)app_can_send_std((uint16_t)app_can_make_id(s_node_id, cmd), data, len);
}

/** @brief 执行 MIT（compact 与 dual 已统一为物理量） */
static void can_exec_mit(float p_deg, float v_rpm, float kp, float kd, float t_ff) {
    s_last_result =
        can_result_code(app_motion_mit(p_deg * CAN_RAD_PER_DEG, can_rpm_to_rad_s(v_rpm), kp, kd, t_ff));
}

/** @brief 执行一帧控制命令（全部非阻塞） */
static void can_exec_frame(const app_can_frame_t* frame) {
    const app_software_params_t* sw = app_software_params_current();

    switch (frame->kind) {
    case APP_CAN_FRAME_AXIS_STATE:
        if (frame->u.axis_state == APP_CAN_AXIS_IDLE) {
            /* 与终端 `foc off` 同语义：只发起受控停机请求，由 app_foc_run_once
             * 推进减速 → 归零 → 软关断（先关 +12V 再停 PWM）。
             * 不在此直接 app_foc_disable()：那会跳过缓降，高速时等于突然断电。 */
            s_last_result = can_result_code(app_motion_stop());
            app_can_service_flush_rx(); /* 丢弃停机期间积压的陈旧 setpoint */
        } else if (frame->u.axis_state == APP_CAN_AXIS_CLOSED_LOOP) {
            s_last_result = can_result_code(app_foc_enable());
        }
        break;
    case APP_CAN_FRAME_CONTROLLER_MODE:
        s_control_mode = frame->u.mode.control_mode;
        s_input_mode = frame->u.mode.input_mode;
        break;
    case APP_CAN_FRAME_INPUT_POS:
        if (s_control_mode == APP_CAN_MODE_POSITION) {
            s_last_result = can_result_code(app_motion_position_multi(
                frame->u.pos.pos_deg * CAN_RAD_PER_DEG,
                can_rpm_to_rad_s(frame->u.pos.vel_ff_rpm), frame->u.pos.torque_ff_nm));
        }
        break;
    case APP_CAN_FRAME_INPUT_VEL:
        if (s_control_mode == APP_CAN_MODE_VELOCITY) {
            s_last_result = can_result_code(app_motion_speed(can_rpm_to_rad_s(frame->u.vel.vel_rpm)));
        }
        break;
    case APP_CAN_FRAME_INPUT_TORQUE:
        if (s_control_mode == APP_CAN_MODE_TORQUE) {
            s_last_result = can_result_code(app_motion_torque(frame->u.torque_nm));
        }
        break;
    case APP_CAN_FRAME_LIMITS:
        if (frame->u.limits.vel_limit_rpm > 0.0f) {
            s_last_result = can_result_code(
                app_motion_set_speed_limit(can_rpm_to_rad_s(frame->u.limits.vel_limit_rpm)));
        }
        if (frame->u.limits.current_limit_a > 0.0f) {
            s_last_result = can_result_code(app_motion_set_iq_max(frame->u.limits.current_limit_a));
        }
        break;
    case APP_CAN_FRAME_MIT:
        if (s_control_mode == APP_CAN_MODE_MIT) {
            /* compact 的 kp/kd 是 0..1 归一化，按配置上限还原为物理量 */
            can_exec_mit(frame->u.mit.p_deg, frame->u.mit.v_rpm,
                         frame->u.mit.kp * sw->mit.kp_max,
                         frame->u.mit.kd * sw->mit.kd_max, frame->u.mit.t_ff);
        }
        break;
    case APP_CAN_FRAME_CLEAR_ERRORS:
        if ((frame->u.identify == 0U) || (frame->u.identify == app_fault_get_first())) {
            s_last_result = can_result_code(app_fault_clear());
        }
        break;
    default:
        break;
    }
}

/** @brief dual MIT B 帧到达：与暂存的 A 配对 */
static void can_try_mit_pair(const app_can_mit_b_t* b, uint32_t now) {
    uint32_t window = CAN_MIT_PAIR_TIMEOUT_MS * s_cycles_per_ms;

    if (!s_mit_a.valid || (s_mit_a.seq != b->seq)
        || ((uint32_t)(now - s_mit_a.cycles) > window)) {
        s_mit_a.valid = false;
        s_stats.mit_pair_timeout++;
        return;
    }
    s_mit_a.valid = false;
    s_stats.mit_pair_ok++;
    if (s_control_mode == APP_CAN_MODE_MIT) {
        can_exec_mit(s_mit_a.p_deg, s_mit_a.v_rpm, b->kp, b->kd, b->t_ff);
    }
}

/** @brief 处理接收队列（有界） */
static void can_process_rx(void) {
    uint32_t now = intf_clock_get_cycle();

    for (uint8_t n = 0U; (n < CAN_RX_BUDGET_PER_STEP) && (s_rx_tail != s_rx_head); n++) {
        can_rx_item_t item = s_rxq[s_rx_tail];

        s_rx_tail = (uint8_t)((s_rx_tail + 1U) % CAN_RX_QUEUE_SIZE);

        uint8_t node = 0U;
        uint8_t cmd = 0U;

        if (!app_can_parse_id(item.id, &node, &cmd) || (node != s_node_id)) {
            continue;
        }
        /* MIT 双帧需要单独配对（不进通用解码） */
        if ((cmd == APP_CAN_CMD_SET_INPUT_MIT_A) || (cmd == APP_CAN_CMD_SET_INPUT_MIT_B)) {
            if (s_mit_format != 1U) {
                s_stats.format_mismatch++;
                continue;
            }
            if (cmd == APP_CAN_CMD_SET_INPUT_MIT_A) {
                app_can_mit_a_t a;

                if (app_can_decode_mit_a(item.data, item.len, &a) != 0) {
                    s_stats.bad_len++;
                    continue;
                }
                s_mit_a.valid = true;
                s_mit_a.seq = a.seq;
                s_mit_a.p_deg = a.p_deg;
                s_mit_a.v_rpm = a.v_rpm;
                s_mit_a.cycles = now;
            } else {
                app_can_mit_b_t b;

                if (app_can_decode_mit_b(item.data, item.len, &b) != 0) {
                    s_stats.bad_len++;
                    continue;
                }
                can_try_mit_pair(&b, now);
            }
            s_stats.rx_frames++;
            continue;
        }
        if ((cmd == APP_CAN_CMD_SET_INPUT_MIT) && (s_mit_format != 0U)) {
            s_stats.format_mismatch++;
            continue;
        }

        app_can_frame_t frame;

        int rc = app_can_decode(item.id, item.data, item.len, s_node_id, &frame);

        if (rc < 0) {
            s_stats.bad_len++;
            continue;
        }
        if (rc > 0) {
            if (frame.kind == APP_CAN_FRAME_UNKNOWN) {
                s_stats.unknown_cmd++;
            }
            continue;
        }
        s_stats.rx_frames++;
        if (frame.kind == APP_CAN_FRAME_ESTOP) {
            /* 急停：本拍内立即执行（先于其它帧的下一轮处理） */
            s_estop_pending = true;
            break;
        }
        can_exec_frame(&frame);
    }

    /* dual MIT 配对超时清理 */
    if (s_mit_a.valid && ((uint32_t)(now - s_mit_a.cycles) > (CAN_MIT_PAIR_TIMEOUT_MS * s_cycles_per_ms))) {
        s_mit_a.valid = false;
        s_stats.mit_pair_timeout++;
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

/** @brief 周期发送：encoder（1kHz）+ heartbeat（100ms） */
static void can_periodic_tx(void) {
    uint8_t data[8];

    if ((s_tick % CAN_ENCODER_TICKS) == 0U) {
        app_motion_snapshot_t snap;

        app_motion_get_snapshot(&snap);
        float out_dir = (snap.output_direction < 0.0f) ? -1.0f : 1.0f;

        (void)app_can_encode_encoder(data, snap.shaft_continuous_rad * out_dir * CAN_DEG_PER_RAD,
                                     snap.speed_rad_s * out_dir * (60.0f / 6.28318530718f));
        can_send(APP_CAN_CMD_GET_ENCODER, data, 8U);
    }
    if ((s_tick % CAN_HEARTBEAT_TICKS) == 0U) {
        app_foc_state_t state = app_foc_get_state();
        uint8_t axis_state =
            ((state == APP_FOC_STATE_READY) || (state == APP_FOC_STATE_RUN))
                ? (uint8_t)APP_CAN_AXIS_CLOSED_LOOP
                : (uint8_t)APP_CAN_AXIS_IDLE;
        /* proc_result 回报最近一条控制命令的拒绝码：主机可据此区分
         * "未使能/模式不符/行程超限/MIT 禁用"等，而不必回连终端。 */
        uint8_t n =
            app_can_encode_heartbeat(data, app_fault_get_latched(), axis_state, s_last_result, 0U);

        can_send(APP_CAN_CMD_HEARTBEAT, data, n);
    }
}

void app_can_service_init(void) {
    const app_software_params_t* sw = app_software_params_current();

    s_node_id = sw->can.node_id_default;
    if ((s_node_id == 0U) || (s_node_id > APP_CAN_NODE_ID_MAX)) {
        s_node_id = 1U;
    }
    s_mit_format = (sw->mit.can_format != 0U) ? 1U : 0U;
    s_timeout_ms = sw->comm.comm_timeout_ms;
    s_cycles_per_ms = intf_clock_get_cpu_freq() / 1000U;
    s_rx_head = 0U;
    s_rx_tail = 0U;
    s_wd_armed = false;
    s_wd_suspended = false;
    s_wd_fired = false;
    s_tick = 0U;
    s_control_mode = APP_CAN_MODE_TORQUE;
    s_input_mode = 0U;
    s_estop_pending = false;
    s_mit_a.valid = false;
    s_last_result = 0U;
    memset(&s_stats, 0, sizeof(s_stats));
    s_last_control_cycles = intf_clock_get_cycle();

    app_can_set_rx_callback(can_rx_callback);
    app_can_set_rx_isr_hook(can_rx_isr_hook);
    /* 接受全部标准帧（节点过滤在软件层做，避免过滤器把调试帧一起吞掉） */
    (void)app_can_add_std_filter(0x000U, 0x000U);
    s_initialized = true;
}

void app_can_service_step(void) {
    if (!s_initialized) {
        return;
    }
    /* 1) 分发 RX（入队） */
    app_can_poll();

    /* 2) 急停优先 */
    if (s_estop_pending) {
        s_estop_pending = false;
        (void)app_motion_stop();
        app_foc_disable();
        app_can_service_flush_rx();
    }

    /* 3) 解析与执行（有界） */
    can_process_rx();

    /* 4) 看门狗 */
    can_watchdog_step();

    /* 5) 周期发送 */
    can_periodic_tx();

    s_tick++;
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

void app_can_service_flush_rx(void) {
    s_rx_tail = s_rx_head;
    s_mit_a.valid = false;
}

uint8_t app_can_service_get_node_id(void) { return s_node_id; }

uint32_t app_can_service_get_dropped(void) { return s_stats.rx_dropped; }

void app_can_service_get_stats(app_can_service_stats_t* out) {
    if (out != NULL) {
        *out = s_stats;
    }
}