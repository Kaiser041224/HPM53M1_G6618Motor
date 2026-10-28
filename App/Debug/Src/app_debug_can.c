/**
 * @file    app_debug_can.c
 * @brief   CAN 自检（MCAN3，经典 CAN）
 * @author  Kaiser
 *
 * 测试内容：
 *   1) 内部环回自检：临时切到 LOOPBACK_INTERNAL，发 0x114 并校验回环数据（内部常量，测试隔离）
 *   2) 正常模式：每秒发送一帧参数回报帧（ID 取自 config/software.yaml → software.can.tx_report_id，
 *      8 字节，首字节递增计数）
 *   3) 接收：全接收过滤器 + RX 回调，收到帧即打印到 RTT
 *   4) 状态：每秒打印错误计数 / bus off / 收发统计
 *
 * 硬件：MCAN3（PA15=TXD / PA14=RXD）→ TPT1044VQ，120Ω 端接由 DIP RES_CTL 控制。
 * 说明：无外部节点时正常模式发送将因无 ACK 而报错并最终 bus off，属预期现象。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_debug_can.h"

#include "app_can.h"
#include "app_can_service.h"
#include "app_debug_rtt.h"
#include "app_software_params.h"
#include "intf_can.h"
#include "intf_clock.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* 驱动注册（App 层不得包含 hpm_* 头文件，沿用既有 extern 约定） */
extern void hpm_can_driver_register(void);

#define CAN_INST         (3U)       /* MCAN3 */
#define CAN_LB_BAUDRATE  (1000000U) /* 环回自检专用（与总线配置无关） */
#define CAN_LB_ID        (0x114U)   /* 环回自检专用 */
#define CAN_DLC          (8U)
#define CAN_TX_PERIOD_MS (1000U)
#define CAN_LB_WAIT_MS   (100U)

static uint32_t s_tick;
static uint32_t s_last_tx_cycle;

/* ============================================================================
 * 内部环回自检（不经外部收发器，不依赖总线对端）
 * ============================================================================ */

static volatile bool s_lb_done;
static intf_can_frame_t s_lb_frame;

/**
 * @brief 环回自检中断回调：收到 RX FIFO0 新报文时保存帧并置完成标志
 * @param inst CAN 实例（未使用）
 * @param events 事件位
 * @param user_data 用户数据（未使用）
 */
static void can_lb_irq_cb(intf_can_inst_t inst, uint32_t events, void* user_data) {
    (void)inst;
    (void)user_data;

    if ((events & INTF_CAN_EVENT_RX_FIFO0_NEW_MSG) != 0U) {
        intf_can_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        if (intf_can_receive_nonblocking(CAN_INST, &frame) == 0) {
            s_lb_frame = frame;
            s_lb_done = true;
        }
    }
}

/**
 * @brief 内部环回自检：临时切到 LOOPBACK_INTERNAL 自发自收并比对数据
 * @return true = 环回成功
 */
static bool can_loopback_selfcheck(void) {
    intf_can_cfg_t cfg = {
        .baudrate = CAN_LB_BAUDRATE,
        .mode = INTF_CAN_MODE_LOOPBACK_INTERNAL,
        .enable_canfd = false,
        .interrupt_mask = INTF_CAN_EVENT_RX_FIFO0_NEW_MSG,
    };
    intf_can_filter_elem_t filter = {
        .type = INTF_CAN_FILTER_CLASSIC,
        .target_fifo = INTF_CAN_FILTER_FIFO0,
        .id = 0U,
        .mask = 0U,
    };
    intf_can_frame_t tx;
    bool ok = false;

    if (intf_can_init(CAN_INST, &cfg) != 0) {
        return false;
    }
    if (intf_can_config_irq_callback(CAN_INST, can_lb_irq_cb, NULL) != 0) {
        intf_can_deinit(CAN_INST);
        return false;
    }
    if (intf_can_config_filter(CAN_INST, 0U, &filter) != 0) {
        intf_can_config_irq_callback(CAN_INST, NULL, NULL);
        intf_can_deinit(CAN_INST);
        return false;
    }

    memset(&tx, 0, sizeof(tx));
    tx.id = CAN_LB_ID;
    tx.frame_type = INTF_CAN_FRAME_CLASSIC;
    tx.dlc = CAN_DLC;
    for (uint8_t i = 0U; i < CAN_DLC; i++) {
        tx.data[i] = (uint8_t)(0x10U + i);
    }

    s_lb_done = false;
    memset((void*)&s_lb_frame, 0, sizeof(s_lb_frame));

    if (intf_can_send(CAN_INST, &tx, 100U) == 0) {
        uint32_t waited;

        for (waited = 0U; (waited < CAN_LB_WAIT_MS) && !s_lb_done; waited++) {
            intf_clock_delay_ms(1U);
        }
        ok = s_lb_done && (s_lb_frame.id == CAN_LB_ID) && (s_lb_frame.dlc == CAN_DLC)
          && (memcmp(s_lb_frame.data, tx.data, CAN_DLC) == 0);
    }

    intf_can_config_irq_callback(CAN_INST, NULL, NULL);
    intf_can_deinit(CAN_INST);
    return ok;
}

/* ============================================================================
 * 自检入口
 * ============================================================================ */

void app_debug_can_init(void) {
    bool lb_ok;
    const app_software_params_t* software;

    /* 先注册驱动：环回自检经 Interface 调用，需要 ops 已就绪（幂等） */
    hpm_can_driver_register();
    software = app_software_params_current(); /* config/software.yaml（总线波特率） */

    app_debug_printf(
        "\r\n[CAN] self-test: MCAN3 (PA15=TXD/PA14=RXD), classic @%u bps, report TX ID=0x%03X\r\n",
        (unsigned)software->can.baudrate, (unsigned)software->can.tx_report_id);

    lb_ok = can_loopback_selfcheck();
    app_debug_printf("[CAN] loopback self-check: %s\r\n", lb_ok ? "OK" : "FAILED");

    if (app_can_init() != 0) {
        app_debug_printf("[CAN] init FAILED\r\n");
        return;
    }
    app_debug_printf("[CAN] init OK, clk=%u Hz\r\n", (unsigned)app_can_get_clock_hz());

    app_can_clear_stats();
    /* RX 回调、ISR 钩子与过滤器所有权已移交 App/Comm/can（app_can_service_init）。
     * 本模块只保留自检与统计打印，避免同一回调/过滤器被两处注册。 */
}

void app_debug_can_run_once(void) {
    uint32_t now = intf_clock_get_cycle();
    uint32_t period_cycles = (intf_clock_get_cpu_freq() / 1000U) * CAN_TX_PERIOD_MS;
    intf_can_status_t st;
    app_can_stats_t stats;
    app_can_service_stats_t proto;

    /* RX 分发与周期发送由 app_can_service_step() 负责（1kHz）。
     * 本函数只做低速统计打印，避免与协议层争用 poll。 */
    if ((uint32_t)(now - s_last_tx_cycle) < period_cycles) {
        return;
    }
    s_last_tx_cycle = now;
    s_tick++;

#if APP_DEBUG_PERIODIC_PRINT
    if ((app_can_get_status(&st) == 0) && (app_can_get_stats(&stats) == 0)) {
        app_can_service_get_stats(&proto);
        app_debug_printf(
            "[CAN] t=%u bus_off=%d | tx_ok=%u rx=%u drop=%u | proto rx=%u drop=%u bad=%u "
            "unk=%u fmt=%u mit(ok=%u to=%u)\r\n",
            (unsigned)s_tick, st.bus_off, (unsigned)stats.tx_ok_count,
            (unsigned)stats.rx_count, (unsigned)stats.rx_drop_count, (unsigned)proto.rx_frames,
            (unsigned)proto.rx_dropped, (unsigned)proto.bad_len, (unsigned)proto.unknown_cmd,
            (unsigned)proto.format_mismatch, (unsigned)proto.mit_pair_ok,
            (unsigned)proto.mit_pair_timeout);
    }
#else
    (void)st;
    (void)stats;
    (void)proto;
#endif
}
