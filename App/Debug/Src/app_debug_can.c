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
static uint32_t s_tx_seq;
static uint32_t s_rx_total;
static uint32_t s_last_tx_cycle;
static uint32_t s_tx_report_id;

/**
 * @brief 将一帧 CAN 报文（ID/DLC/数据）打印到 RTT
 * @param tag 行首标记（如 "rx"）
 * @param msg 待打印报文
 */
static void can_dump_frame(const char* tag, const app_can_msg_t* msg) {
    app_debug_printf("[CAN] %s ID=0x%03lX DLC=%u data=", tag, (unsigned long)msg->id, msg->dlc);
    for (uint8_t i = 0U; (i < msg->dlc) && (i < 8U); i++) {
        app_debug_printf("%02X ", msg->data[i]);
    }
    app_debug_printf("\r\n");
}

/**
 * @brief CAN 接收回调：打印并把收到的帧原样回发（回显验证）
 * @param msg 收到的报文
 */
static void can_rx_callback(const app_can_msg_t* msg) {
    int echo_ret;

    s_rx_total++;
    can_dump_frame("rx", msg);

    /*
     * 回显：把收到的帧原样发回总线（对应 UART 自检的回显验证）。
     * 上位机应能收到同 ID / 同数据的帧；失败时打印错误。
     */
    if (msg->is_ext_id) {
        echo_ret = app_can_send_ext(msg->id, msg->data, msg->dlc);
    } else {
        echo_ret = app_can_send_std((uint16_t)msg->id, msg->data, msg->dlc);
    }
    if (echo_ret != 0) {
        app_debug_printf("[CAN] echo FAILED ret=%d\r\n", echo_ret);
    }
}

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
    app_software_params_t software;

    /* 先注册驱动：环回自检经 Interface 调用，需要 ops 已就绪（幂等） */
    hpm_can_driver_register();
    app_software_params_load(&software); /* config/software.yaml（总线波特率 + 参数回报帧 ID） */
    s_tx_report_id = software.can.tx_report_id;

    app_debug_printf(
        "\r\n[CAN] self-test: MCAN3 (PA15=TXD/PA14=RXD), classic @%u bps, report TX ID=0x%03X\r\n",
        (unsigned)software.can.baudrate, (unsigned)software.can.tx_report_id);

    lb_ok = can_loopback_selfcheck();
    app_debug_printf("[CAN] loopback self-check: %s\r\n", lb_ok ? "OK" : "FAILED");

    if (app_can_init() != 0) {
        app_debug_printf("[CAN] init FAILED\r\n");
        return;
    }
    app_debug_printf("[CAN] init OK, clk=%u Hz\r\n", (unsigned)app_can_get_clock_hz());

    app_can_set_rx_callback(can_rx_callback);
    app_can_clear_stats();

    if (app_can_add_std_filter(0x000U, 0x000U) != 0) {
        app_debug_printf("[CAN] std filter config FAILED\r\n");
    }
    if (app_can_add_ext_filter(0x00000000U, 0x00000000U) != 0) {
        app_debug_printf("[CAN] ext filter config FAILED\r\n");
    }
}

void app_debug_can_run_once(void) {
    uint32_t now = intf_clock_get_cycle();
    uint32_t period_cycles = (intf_clock_get_cpu_freq() / 1000U) * CAN_TX_PERIOD_MS;

    /* 1) 分发接收回调（主循环上下文） */
    app_can_poll();

    /* 2) 每秒发送一帧 + 状态行 */
    if ((uint32_t)(now - s_last_tx_cycle) >= period_cycles) {
        uint8_t data[CAN_DLC];
        intf_can_status_t st;
        app_can_stats_t stats;
        int ret;

        s_last_tx_cycle = now;
        s_tick++;

        data[0] = (uint8_t)s_tx_seq;
        for (uint8_t i = 1U; i < CAN_DLC; i++) {
            data[i] = (uint8_t)(0x10U + i);
        }

        /* 参数回报帧 ID（config/software.yaml；11-bit 标准帧，SCHEMA 限
         * ≤0x7FF）；负载为计数占位，非真实参数回报 */
        ret = app_can_send_std((uint16_t)s_tx_report_id, data, CAN_DLC);
        s_tx_seq++;

#if APP_DEBUG_PERIODIC_PRINT
        if ((app_can_get_status(&st) == 0) && (app_can_get_stats(&stats) == 0)) {
            app_debug_printf(
                "[CAN] tx tick=%u seq=%u ret=%d rx_total=%u | tx_err=%u rx_err=%u bus_off=%d | "
                "tx_ok=%u rx=%u drop=%u\r\n",
                (unsigned)s_tick, (unsigned)data[0], ret, (unsigned)s_rx_total, st.tx_error_count,
                st.rx_error_count, st.bus_off, (unsigned)stats.tx_ok_count,
                (unsigned)stats.rx_count, (unsigned)stats.rx_drop_count);
        }
#else
        (void)ret;
        (void)st;
        (void)stats;
#endif
    }
}
