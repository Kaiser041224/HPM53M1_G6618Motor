/**
 * @file    app_debug_foc.c
 * @brief   FOC 调试/观测结构实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_debug_foc.h"

#include "app_debug_encoder.h"
#include "app_encoder.h"
#include "app_fault.h"
#include "app_foc.h"
#include "app_foc_current.h"
#include "app_motor_identify.h"
#include "intf_clock.h"

/** Ozone 结构（.noncacheable.bss：启动清零 + 调试器直读，不受 D-Cache 影响） */
volatile app_debug_foc_t g_app_debug_foc
    __attribute__((section(".noncacheable.bss")));

/** 每拍最多处理的一次性命令数（有界，避免长时间占用主循环） */
#define APP_DEBUG_FOC_MAX_CMDS_PER_TICK (2U)

/** 上次已应用的调试目标（仅当目标变化时才写 app_foc，避免覆盖终端/辨识设定） */
static float s_last_iq_target;
static float s_last_id_target;

/**
 * @brief 执行一条一次性命令
 * @param cmd 命令
 * @return 命令返回值（0 = 成功）
 */
static int32_t app_debug_foc_dispatch(uint32_t cmd) {
    switch (cmd) {
    case APP_DEBUG_FOC_CMD_ENABLE:
        /* 先清零目标给定：避免上一次运行遗留转矩在 enable 后立即重启 */
        g_app_debug_foc.iq_target_a = 0.0f;
        g_app_debug_foc.id_target_a = 0.0f;
        return (int32_t)app_foc_enable();
    case APP_DEBUG_FOC_CMD_DISABLE:
        g_app_debug_foc.iq_target_a = 0.0f;
        g_app_debug_foc.id_target_a = 0.0f;
        app_foc_disable();
        return 0;
    case APP_DEBUG_FOC_CMD_CLEAR_FAULT:
        return (int32_t)app_fault_clear();
    case APP_DEBUG_FOC_CMD_VTEST: {
        float volts = (float)g_app_debug_foc.arg0 / 1000.0f;
        float theta = (float)g_app_debug_foc.arg1 / 10000.0f;
        app_foc_state_t st = app_foc_get_state();

        if ((st != APP_FOC_STATE_READY) && (st != APP_FOC_STATE_RUN)) {
            return -3; /* 仅 READY/RUN（FAULT/OFF/CALIB 不允许开环输出） */
        }
        return (int32_t)app_foc_current_vtest_start(volts, theta, 1.0f);
    }
    case APP_DEBUG_FOC_CMD_CAL_ENCODER:
        return (int32_t)app_motor_identify_start();
    case APP_DEBUG_FOC_CMD_CAL_ABORT:
        app_motor_identify_abort();
        return 0;
    case APP_DEBUG_FOC_CMD_PASSIVE_SELFTEST:
        /* 运行期健康自检：只评估采样器快照流（不触碰 SPI3），100ms 后出结果 */
        if (app_debug_encoder_health_active()) {
            return -1; /* 已在进行中 */
        }
        app_debug_encoder_health_start();
        return 0;
    default:
        return -1;
    }
}

/**
 * @brief 刷新状态域（快照 + 编码器 + 辨识 + 故障）
 */
static void app_debug_foc_refresh(void) {
    app_foc_current_snapshot_t snap;
    app_encoder_rotor_snapshot_t enc;
    app_motor_identify_result_t idr;
    uint32_t cpu = intf_clock_get_cpu_freq();

    app_foc_get_snapshot(&snap);
    (void)app_encoder_get_rotor_snapshot(&enc);
    app_motor_identify_get_result(&idr);

    g_app_debug_foc.state = (uint32_t)app_foc_get_state();
    g_app_debug_foc.enabled = app_foc_is_active() ? 1U : 0U;
    g_app_debug_foc.fault_codes = app_fault_get_codes();
    g_app_debug_foc.fault_latched = app_fault_get_latched();
    g_app_debug_foc.tripped = app_foc_current_is_tripped() ? 1U : 0U;
    g_app_debug_foc.saturated = snap.saturated ? 1U : 0U;

    g_app_debug_foc.isr_cycles = g_foc_isr_cycles;
    g_app_debug_foc.isr_cycles_max = g_foc_isr_cycles_max;
    g_app_debug_foc.isr_overruns = g_foc_isr_overruns;
    g_app_debug_foc.isr_run_count = snap.run_count;

    g_app_debug_foc.enc_seq = enc.seq;
    g_app_debug_foc.enc_age_cycles = enc.age_cycles;
    g_app_debug_foc.enc_age_us =
        (cpu != 0U) ? (uint32_t)(((uint64_t)enc.age_cycles * 1000000U) / cpu) : 0U;
    g_app_debug_foc.enc_errors = app_encoder_get_error_count(APP_ENCODER_ROTOR);
    g_app_debug_foc.enc_jumps = app_encoder_get_rotor_jump_count();
    g_app_debug_foc.enc_read_fail = g_encoder_read_fail_count;
    g_app_debug_foc.enc_isr_cycles = g_encoder_isr_cycles;
    g_app_debug_foc.enc_isr_cycles_max = g_encoder_isr_cycles_max;

    g_app_debug_foc.estop_ack = g_foc_fault_request;
    g_app_debug_foc.isr_inhibited = app_foc_isr_inhibited() ? 1U : 0U;
    g_app_debug_foc.duty_max_eff = app_foc_current_duty_max();

    g_app_debug_foc.selftest_active = g_enc_runtime_active;
    g_app_debug_foc.selftest_ok = g_enc_runtime_ok;
    g_app_debug_foc.selftest_progress = g_enc_runtime_progress;
    g_app_debug_foc.selftest_seq_delta = g_enc_runtime_seq_delta;
    g_app_debug_foc.selftest_valid_pct = g_enc_runtime_valid_pct;
    g_app_debug_foc.selftest_read_fail_delta = g_enc_runtime_read_fail_delta;
    g_app_debug_foc.selftest_jump_delta = g_enc_runtime_jump_delta;
    g_app_debug_foc.selftest_age_max_us = g_enc_runtime_age_max_us;

    g_app_debug_foc.cal_active = idr.active ? 1U : 0U;
    g_app_debug_foc.cal_done = idr.done ? 1U : 0U;
    g_app_debug_foc.cal_failed = idr.failed ? 1U : 0U;
    g_app_debug_foc.cal_fail_reason = (uint32_t)idr.fail_reason;
    g_app_debug_foc.cal_progress = idr.progress;
    g_app_debug_foc.cal_offset_rad = idr.offset_rad;
    g_app_debug_foc.cal_direction = idr.direction;
    g_app_debug_foc.cal_quality = idr.quality;

    g_app_debug_foc.i_d_a = snap.i_d_a;
    g_app_debug_foc.i_q_a = snap.i_q_a;
    g_app_debug_foc.i_d_avg_a = snap.i_d_avg_a;
    g_app_debug_foc.i_q_avg_a = snap.i_q_avg_a;
    g_app_debug_foc.i_d_ref_a = snap.i_d_ref_a;
    g_app_debug_foc.i_q_ref_a = snap.i_q_ref_a;
    g_app_debug_foc.duty_u = snap.duty_u;
    g_app_debug_foc.duty_v = snap.duty_v;
    g_app_debug_foc.duty_w = snap.duty_w;
    g_app_debug_foc.v_bus_v = snap.v_bus_v;
    g_app_debug_foc.v_scale = snap.v_scale;
    g_app_debug_foc.theta_e_rad = snap.theta_e_rad;
    g_app_debug_foc.omega_e_rad_s = snap.omega_e_rad_s;
}

void app_debug_foc_init(void) {
    /* 请求/状态清零（调试器可在 init 后立即读） */
    g_app_debug_foc = (app_debug_foc_t){0};
    g_app_debug_foc.ready = 1U;

    /* 绑定紧急停机邮箱：ISR 每拍只读 g_app_debug_foc.estop_request，
     * 无需主循环参与；Ozone 写 1 即停机。见 app_foc_register_estop_request。 */
    app_foc_register_estop_request(&g_app_debug_foc.estop_request);
}

void app_debug_foc_tick(void) {
    uint32_t i;

    g_app_debug_foc.tick_count++;

    /* 1) 有界处理命令 */
    for (i = 0U; i < APP_DEBUG_FOC_MAX_CMDS_PER_TICK; i++) {
        uint32_t cmd = g_app_debug_foc.command;

        if (cmd == (uint32_t)APP_DEBUG_FOC_CMD_NONE) {
            break;
        }
        g_app_debug_foc.ack_result = app_debug_foc_dispatch(cmd);
        g_app_debug_foc.command = (uint32_t)APP_DEBUG_FOC_CMD_NONE;
        g_app_debug_foc.ack_sequence++;
    }

    /* 2) 运行期给定消费：仅 READY/RUN，且仅在调试目标"变化"时写入
     *    （避免每拍覆盖终端/辨识设定的给定；enable/disable 已清零目标） */
    {
        app_foc_state_t st = app_foc_get_state();
        float iq = g_app_debug_foc.iq_target_a;
        float id = g_app_debug_foc.id_target_a;

        if ((st == APP_FOC_STATE_READY) || (st == APP_FOC_STATE_RUN)) {
            if (iq != s_last_iq_target) {
                if (app_foc_set_iq_ref(iq) == 0) {
                    s_last_iq_target = iq;
                }
            }
            if (id != s_last_id_target) {
                if (app_foc_set_id_ref(id) == 0) {
                    s_last_id_target = id;
                }
            }
        } else {
            /* 非活动态：同步缓存，激活后不会重放陈旧目标 */
            s_last_iq_target = iq;
            s_last_id_target = id;
        }
    }

    /* 3) 电气标定 1kHz 推进（集中于此，无 Terminal 依赖；台架模式同样生效） */
    if (app_motor_identify_is_active()) {
        app_motor_identify_run_once(g_app_debug_foc.tick_count);
    }

    /* 3b) 运行期健康自检推进（不触碰 SPI3；100ms 窗口） */
    app_debug_encoder_health_tick();

    /* 4) 刷新状态域 */
    app_debug_foc_refresh();
}
