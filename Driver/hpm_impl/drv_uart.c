/**
 * @file    drv_uart.c
 * @brief   UART 驱动 - HPM UART 适配（轮询 TX + 中断 RX）
 * @author  Kaiser
 *
 * 实现策略：
 *   - TX：轮询（LSR.THRE），带毫秒级超时（基于 mcycle，见 intf_clock）
 *   - RX：中断接收；ISR 将字节写入每端口 SPSC 环形缓冲，
 *         可选回调（intf_uart_t::register_rx_callback）在中断上下文执行
 *   - 端口映射：port 0..3 -> HPM_UART0..UART3（本板 UART0 = PA00/PA01）
 *
 * intf_uart_cfg_t 字段约定（Interface 未定义枚举，此处固定语义）：
 *   data_bits : 5..8，0 = 默认 8
 *   stop_bits : 1..2，0 = 默认 1
 *   parity    : 0=无校验 1=奇校验 2=偶校验（与 SDK parity 枚举一致）
 *
 * timeout_ms 语义（transmit / receive 通用）：
 *   0          = 不等待（立即返回当前状态 / 已收数据）
 *   UINT32_MAX = 无限等待
 *   其他       = 毫秒级超时
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_uart.h"
#include "intf_clock.h"

#include "hpm_uart_drv.h"
#include "hpm_clock_drv.h"
#include "hpm_interrupt.h"
#include "hpm_soc_irq.h"

#include <stddef.h>

#define UART_INSTANCE_COUNT (4U)
#define UART_RX_RING_SIZE   (256U) /* 必须为 2 的幂 */
#define UART_RX_RING_MASK   (UART_RX_RING_SIZE - 1U)
#define UART_ISR_CHUNK_SIZE (16U)

/**
 * @brief UART 实例上下文
 */
typedef struct {
    UART_Type        *base;        /**< UART 寄存器基地址 */
    clock_name_t      clock;       /**< 外设时钟 */
    uint32_t          irq;         /**< 中断号 */
    intf_uart_rx_cb_t rx_cb;       /**< 接收回调（中断上下文，可为 NULL） */
    bool              initialized; /**< 是否已初始化 */
} uart_ctx_t;

static uart_ctx_t s_uart_ctx[UART_INSTANCE_COUNT] = {
    { .base = HPM_UART0, .clock = clock_uart0, .irq = IRQn_UART0 },
    { .base = HPM_UART1, .clock = clock_uart1, .irq = IRQn_UART1 },
    { .base = HPM_UART2, .clock = clock_uart2, .irq = IRQn_UART2 },
    { .base = HPM_UART3, .clock = clock_uart3, .irq = IRQn_UART3 },
};

/* SPSC 环形缓冲：head 仅 ISR 写，tail 仅主循环写（RV32 上 16bit 访问原子） */
static uint8_t           s_rx_buf[UART_INSTANCE_COUNT][UART_RX_RING_SIZE];
static volatile uint16_t s_rx_head[UART_INSTANCE_COUNT];
static volatile uint16_t s_rx_tail[UART_INSTANCE_COUNT];

/**
 * @brief 向 SPSC 环形缓冲压入一个字节（满则丢弃）
 * @param port 端口号
 * @param byte 待压入字节
 */
static inline void uart_ring_push(uint8_t port, uint8_t byte)
{
    uint16_t next = (uint16_t)((s_rx_head[port] + 1U) & UART_RX_RING_MASK);

    if (next == s_rx_tail[port]) {
        return; /* 满：丢弃新字节 */
    }
    s_rx_buf[port][s_rx_head[port] & UART_RX_RING_MASK] = byte;
    s_rx_head[port] = next;
}

/**
 * @brief 从 SPSC 环形缓冲弹出一个字节
 * @param port 端口号
 * @param byte 输出字节
 * @return true = 取到数据
 */
static inline bool uart_ring_pop(uint8_t port, uint8_t *byte)
{
    if (s_rx_head[port] == s_rx_tail[port]) {
        return false;
    }
    *byte = s_rx_buf[port][s_rx_tail[port] & UART_RX_RING_MASK];
    s_rx_tail[port] = (uint16_t)((s_rx_tail[port] + 1U) & UART_RX_RING_MASK);
    return true;
}

/* timeout_ms 语义：0 = 不等待；UINT32_MAX = 无限等待；其他 = 毫秒超时 */
/**
 * @brief 毫秒转 CPU cycle
 * @param ms 毫秒数
 * @return 对应 cycle 数
 */
static inline uint32_t uart_ms_to_cycles(uint32_t ms)
{
    return (uint32_t)((uint64_t) ms * (intf_clock_get_cpu_freq() / 1000U));
}

/**
 * @brief 判断超时是否到达
 * @param start 起始 cycle
 * @param timeout_cycles 超时 cycle
 * @param timeout_ms 超时毫秒语义
 * @return true = 已超时
 */
static inline bool uart_timeout_elapsed(uint32_t start, uint32_t timeout_cycles,
                                        uint32_t timeout_ms)
{
    if (timeout_ms == 0U) {
        return true; /* 不等待 */
    }
    if (timeout_ms == UINT32_MAX) {
        return false; /* 无限等待 */
    }
    return (uint32_t)(intf_clock_get_cycle() - start) >= timeout_cycles;
}

/* ============================================================================
 * 中断接收
 * ============================================================================ */

/**
 * @brief UART 端口中断服务：收字节入环形缓冲并触发回调
 * @param port 端口号
 */
static void uart_port_isr(uint8_t port)
{
    uart_ctx_t *ctx = &s_uart_ctx[port];
    uint8_t irq_id = uart_get_irq_id(ctx->base);

    if ((irq_id == uart_intr_id_rx_data_avail) || (irq_id == uart_intr_id_rx_timeout)) {
        uint8_t chunk[UART_ISR_CHUNK_SIZE];
        uint8_t chunk_len = 0U;

        while ((chunk_len < UART_ISR_CHUNK_SIZE) && uart_check_status(ctx->base, uart_stat_data_ready)) {
            chunk[chunk_len++] = uart_read_byte(ctx->base);
        }
        for (uint8_t i = 0U; i < chunk_len; i++) {
            uart_ring_push(port, chunk[i]);
        }
        if ((chunk_len > 0U) && (ctx->rx_cb != NULL)) {
            ctx->rx_cb(chunk, chunk_len); /* 回调在中断上下文执行，data 仅在回调期间有效 */
        }
    }
}

SDK_DECLARE_EXT_ISR_M(IRQn_UART0, isr_uart0)
void isr_uart0(void) { uart_port_isr(0U); }

SDK_DECLARE_EXT_ISR_M(IRQn_UART1, isr_uart1)
void isr_uart1(void) { uart_port_isr(1U); }

SDK_DECLARE_EXT_ISR_M(IRQn_UART2, isr_uart2)
void isr_uart2(void) { uart_port_isr(2U); }

SDK_DECLARE_EXT_ISR_M(IRQn_UART3, isr_uart3)
void isr_uart3(void) { uart_port_isr(3U); }

/* ============================================================================
 * 实现
 * ============================================================================ */

/**
 * @brief 初始化指定 UART 端口（含中断 RX）
 * @param port 端口号
 * @param cfg UART 配置
 * @return 0 = 成功；-1 = 参数非法或 SDK 初始化失败
 */
static int uart_init_impl(uint8_t port, const intf_uart_cfg_t *cfg)
{
    uart_config_t ucfg = {0};

    if ((port >= UART_INSTANCE_COUNT) || (cfg == NULL)) {
        return -1;
    }

    uart_ctx_t *ctx = &s_uart_ctx[port];

    clock_add_to_group(ctx->clock, 0);
    uart_default_config(ctx->base, &ucfg);

    ucfg.src_freq_in_hz = clock_get_frequency(ctx->clock);
    ucfg.baudrate = cfg->baudrate;
    ucfg.fifo_enable = true;

    switch (cfg->data_bits) {
    case 5U: ucfg.word_length = word_length_5_bits; break;
    case 6U: ucfg.word_length = word_length_6_bits; break;
    case 7U: ucfg.word_length = word_length_7_bits; break;
    case 8U:
    case 0U:
    default: ucfg.word_length = word_length_8_bits; break;
    }

    switch (cfg->parity) {
    case 1U: ucfg.parity = parity_odd; break;
    case 2U: ucfg.parity = parity_even; break;
    case 0U:
    default: ucfg.parity = parity_none; break;
    }

    ucfg.num_of_stop_bits = (cfg->stop_bits == 2U) ? stop_bits_2 : stop_bits_1;
    ucfg.modem_config.auto_flow_ctrl_en = cfg->flow_ctrl;

    if (uart_init(ctx->base, &ucfg) != status_success) {
        return -1;
    }

    s_rx_head[port] = 0U;
    s_rx_tail[port] = 0U;
    ctx->rx_cb = NULL;
    ctx->initialized = true;

    /* 中断 RX：使能数据到达中断并挂入 PLIC */
    uart_enable_irq(ctx->base, uart_intr_rx_data_avail_or_timeout);
    intc_m_enable_irq_with_priority(ctx->irq, 1);

    return 0;
}

/**
 * @brief UART 轮询发送
 * @param port 端口号
 * @param data 发送缓冲
 * @param len 长度 [byte]
 * @param timeout_ms 超时毫秒语义
 * @return 0 = 成功；-1 = 参数非法、未初始化或超时
 */
static int uart_transmit_impl(uint8_t port, const uint8_t *data, size_t len,
                               uint32_t timeout_ms)
{
    if ((port >= UART_INSTANCE_COUNT) || (data == NULL)) {
        return -1;
    }

    uart_ctx_t *ctx = &s_uart_ctx[port];
    uint32_t timeout_cycles;
    uint32_t start;

    if (!ctx->initialized) {
        return -1;
    }

    start = intf_clock_get_cycle();
    timeout_cycles = uart_ms_to_cycles(timeout_ms);

    for (size_t i = 0U; i < len; i++) {
        while (!uart_check_status(ctx->base, uart_stat_tx_slot_avail)) {
            if (uart_timeout_elapsed(start, timeout_cycles, timeout_ms)) {
                return -1;
            }
        }
        uart_write_byte(ctx->base, data[i]);
    }

    return 0;
}

/**
 * @brief UART 从环形缓冲接收
 * @param port 端口号
 * @param data 接收缓冲
 * @param len 期望长度 [byte]
 * @param timeout_ms 超时毫秒语义
 * @return 实际接收字节数；-1 = 参数非法或未初始化
 */
static int uart_receive_impl(uint8_t port, uint8_t *data, size_t len,
                              uint32_t timeout_ms)
{
    if ((port >= UART_INSTANCE_COUNT) || (data == NULL) || (len == 0U)) {
        return -1;
    }

    uart_ctx_t *ctx = &s_uart_ctx[port];
    uint32_t timeout_cycles;
    uint32_t start;
    size_t read_len = 0U;

    if (!ctx->initialized) {
        return -1;
    }

    start = intf_clock_get_cycle();
    timeout_cycles = uart_ms_to_cycles(timeout_ms);

    while (read_len < len) {
        uint8_t byte;

        if (uart_ring_pop(port, &byte)) {
            data[read_len++] = byte;
            continue;
        }
        if (read_len > 0U) {
            break; /* 已有数据，立即返回本次可读部分 */
        }
        if (uart_timeout_elapsed(start, timeout_cycles, timeout_ms)) {
            break;
        }
    }

    return (int) read_len;
}

/**
 * @brief 注册接收回调
 * @param port 端口号
 * @param cb 回调（中断上下文执行）
 * @return 0 = 成功；-1 = 端口越界
 */
static int uart_register_rx_callback_impl(uint8_t port, intf_uart_rx_cb_t cb)
{
    if (port >= UART_INSTANCE_COUNT) {
        return -1;
    }
    s_uart_ctx[port].rx_cb = cb;
    return 0;
}

/**
 * @brief 反初始化指定 UART 端口
 * @param port 端口号
 */
static void uart_deinit_impl(uint8_t port)
{
    if (port >= UART_INSTANCE_COUNT) {
        return;
    }

    uart_ctx_t *ctx = &s_uart_ctx[port];

    if (!ctx->initialized) {
        return;
    }

    uart_disable_irq(ctx->base, uart_intr_rx_data_avail_or_timeout);
    ctx->rx_cb = NULL;
    ctx->initialized = false;
}

/* ============================================================================
 * 每实例设备对象（风格 A）
 * ============================================================================ */

/**
 * @brief UART0 初始化包装
 * @param cfg UART 配置
 * @return 0 = 成功；-1 = 失败
 */
static int uart0_init(const intf_uart_cfg_t *cfg) { return uart_init_impl(0U, cfg); }

/**
 * @brief UART0 发送包装
 * @param data 发送缓冲
 * @param len 长度 [byte]
 * @param timeout_ms 超时毫秒语义
 * @return 0 = 成功；-1 = 失败
 */
static int uart0_transmit(const uint8_t *data, size_t len, uint32_t timeout_ms)
{ return uart_transmit_impl(0U, data, len, timeout_ms); }

/**
 * @brief UART0 接收包装
 * @param data 接收缓冲
 * @param len 期望长度 [byte]
 * @param timeout_ms 超时毫秒语义
 * @return 实际接收字节数；-1 = 失败
 */
static int uart0_receive(uint8_t *data, size_t len, uint32_t timeout_ms)
{ return uart_receive_impl(0U, data, len, timeout_ms); }

/**
 * @brief UART0 注册接收回调
 * @param cb 回调
 * @return 0 = 成功；-1 = 失败
 */
static int uart0_register_rx_callback(intf_uart_rx_cb_t cb)
{ return uart_register_rx_callback_impl(0U, cb); }

/**
 * @brief UART0 反初始化包装
 */
static void uart0_deinit(void) { uart_deinit_impl(0U); }

/**
 * @brief UART1 初始化包装
 * @param cfg UART 配置
 * @return 0 = 成功；-1 = 失败
 */
static int uart1_init(const intf_uart_cfg_t *cfg) { return uart_init_impl(1U, cfg); }

/**
 * @brief UART1 发送包装
 * @param data 发送缓冲
 * @param len 长度 [byte]
 * @param timeout_ms 超时毫秒语义
 * @return 0 = 成功；-1 = 失败
 */
static int uart1_transmit(const uint8_t *data, size_t len, uint32_t timeout_ms)
{ return uart_transmit_impl(1U, data, len, timeout_ms); }

/**
 * @brief UART1 接收包装
 * @param data 接收缓冲
 * @param len 期望长度 [byte]
 * @param timeout_ms 超时毫秒语义
 * @return 实际接收字节数；-1 = 失败
 */
static int uart1_receive(uint8_t *data, size_t len, uint32_t timeout_ms)
{ return uart_receive_impl(1U, data, len, timeout_ms); }

/**
 * @brief UART1 注册接收回调
 * @param cb 回调
 * @return 0 = 成功；-1 = 失败
 */
static int uart1_register_rx_callback(intf_uart_rx_cb_t cb)
{ return uart_register_rx_callback_impl(1U, cb); }

/**
 * @brief UART1 反初始化包装
 */
static void uart1_deinit(void) { uart_deinit_impl(1U); }

/**
 * @brief UART2 初始化包装
 * @param cfg UART 配置
 * @return 0 = 成功；-1 = 失败
 */
static int uart2_init(const intf_uart_cfg_t *cfg) { return uart_init_impl(2U, cfg); }

/**
 * @brief UART2 发送包装
 * @param data 发送缓冲
 * @param len 长度 [byte]
 * @param timeout_ms 超时毫秒语义
 * @return 0 = 成功；-1 = 失败
 */
static int uart2_transmit(const uint8_t *data, size_t len, uint32_t timeout_ms)
{ return uart_transmit_impl(2U, data, len, timeout_ms); }

/**
 * @brief UART2 接收包装
 * @param data 接收缓冲
 * @param len 期望长度 [byte]
 * @param timeout_ms 超时毫秒语义
 * @return 实际接收字节数；-1 = 失败
 */
static int uart2_receive(uint8_t *data, size_t len, uint32_t timeout_ms)
{ return uart_receive_impl(2U, data, len, timeout_ms); }

/**
 * @brief UART2 注册接收回调
 * @param cb 回调
 * @return 0 = 成功；-1 = 失败
 */
static int uart2_register_rx_callback(intf_uart_rx_cb_t cb)
{ return uart_register_rx_callback_impl(2U, cb); }

/**
 * @brief UART2 反初始化包装
 */
static void uart2_deinit(void) { uart_deinit_impl(2U); }

/**
 * @brief UART3 初始化包装
 * @param cfg UART 配置
 * @return 0 = 成功；-1 = 失败
 */
static int uart3_init(const intf_uart_cfg_t *cfg) { return uart_init_impl(3U, cfg); }

/**
 * @brief UART3 发送包装
 * @param data 发送缓冲
 * @param len 长度 [byte]
 * @param timeout_ms 超时毫秒语义
 * @return 0 = 成功；-1 = 失败
 */
static int uart3_transmit(const uint8_t *data, size_t len, uint32_t timeout_ms)
{ return uart_transmit_impl(3U, data, len, timeout_ms); }

/**
 * @brief UART3 接收包装
 * @param data 接收缓冲
 * @param len 期望长度 [byte]
 * @param timeout_ms 超时毫秒语义
 * @return 实际接收字节数；-1 = 失败
 */
static int uart3_receive(uint8_t *data, size_t len, uint32_t timeout_ms)
{ return uart_receive_impl(3U, data, len, timeout_ms); }

/**
 * @brief UART3 注册接收回调
 * @param cb 回调
 * @return 0 = 成功；-1 = 失败
 */
static int uart3_register_rx_callback(intf_uart_rx_cb_t cb)
{ return uart_register_rx_callback_impl(3U, cb); }

/**
 * @brief UART3 反初始化包装
 */
static void uart3_deinit(void) { uart_deinit_impl(3U); }

static const intf_uart_t s_uart0_dev = {
    .instance_id = 0U,
    .init = uart0_init,
    .transmit = uart0_transmit,
    .receive = uart0_receive,
    .register_rx_callback = uart0_register_rx_callback,
    .deinit = uart0_deinit,
};

static const intf_uart_t s_uart1_dev = {
    .instance_id = 1U,
    .init = uart1_init,
    .transmit = uart1_transmit,
    .receive = uart1_receive,
    .register_rx_callback = uart1_register_rx_callback,
    .deinit = uart1_deinit,
};

static const intf_uart_t s_uart2_dev = {
    .instance_id = 2U,
    .init = uart2_init,
    .transmit = uart2_transmit,
    .receive = uart2_receive,
    .register_rx_callback = uart2_register_rx_callback,
    .deinit = uart2_deinit,
};

static const intf_uart_t s_uart3_dev = {
    .instance_id = 3U,
    .init = uart3_init,
    .transmit = uart3_transmit,
    .receive = uart3_receive,
    .register_rx_callback = uart3_register_rx_callback,
    .deinit = uart3_deinit,
};

void hpm_uart_driver_register(void)
{
    intf_uart_register(&s_uart0_dev);
    intf_uart_register(&s_uart1_dev);
    intf_uart_register(&s_uart2_dev);
    intf_uart_register(&s_uart3_dev);
}
