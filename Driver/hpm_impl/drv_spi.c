/**
 * @file    drv_spi.c
 * @brief   SPI 驱动 - HPM SPI 主机适配（每实例设备对象）
 * @author  Kaiser
 *
 * 实现策略：
 *   - 主机模式，轮询收发（快速单帧路径为主，SDK 路径兜底）
 *   - 一次 transfer 调用 = 一个 CS 周期；CS 由控制器硬件自动控制
 *     （cs_index 选择 CS0..CS3，本板两路编码器均用 CS0）
 *   - 时序：sclk 整数分频（SDK 要求偶数分频且 ≤510）；cs2sclk/csht 取 SDK 默认
 *     （csht = 12 半 SCLK，@10MHz = 600ns，满足 KTH7823 Tpause>150ns）
 *   - 时钟自管：clock_add_to_group（幂等），源频取 clock_get_frequency
 *   - 总线忙（status_spi_master_busy）按 timeout_ms 重试；
 *     timeout_ms 语义与 uart/can 一致：0=不等待 / UINT32_MAX=无限 / 毫秒
 *   - 硬件触发单帧（trig_*）：触发矩阵 DMA 请求 → DMAMUX → HDMA 通道（= 总线号）
 *     把常量 0xFF 写入 SPI CMD 启动一帧（UM §47.2.5：主模式写 CMD 即开始传输）；
 *     TX FIFO 预填发送字（UM §47.5.8：多写的数据保留到后续传输；TX 空则暂停），
 *     消费点 trig_fetch 取最新 RX 字并补满 TX FIFO —— CPU 不参与启动、不忙等。
 *     HDMA 通道分配：ch = SPI 总线号（本工程无其它 HDMA 使用者）。
 *
 * 实例映射：bus 0..3 -> HPM_SPI0..SPI3（本板仅 SPI1/SPI3 引出）
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_spi.h"
#include "intf_clock.h"

#include "hpm_spi_drv.h"
#include "hpm_clock_drv.h"
#include "hpm_dmav2_drv.h"
#include "hpm_dmamux_drv.h"
#include "hpm_dmamux_src.h"
#include "hpm_misc.h"

#ifndef ATTR_RAMFUNC
#define ATTR_RAMFUNC __attribute__((section(".fast")))
#endif

#define SPI_INSTANCE_COUNT (4U)

/* 快速单帧路径轮询上限：正常单帧 ~3µs；此处按 ~10µs 设循环上限。
 * 原值 20000（≈0.4ms/次等待）在编码器场景（2 帧 × 3 等待 = 最坏 2.4ms）
 * 一旦搬进 25kHz ADC 中断就会饿死主循环/USB —— 台架表现为 foc on 后终端失联。
 * 注：循环次数 ≠ 时间（每圈含寄存器访问，且可能被更高优先级 ISR 抢占），
 * 因此**另加基于 intf_clock_get_cycle() 的硬期限**（见 SPI_FAST_DEADLINE_US）。 */
#define SPI_FAST_RETRY_MAX (1000U)
/* 单帧等待硬期限 [µs]：无论循环次数如何，超过即失败返回（时间界，非循环界） */
#define SPI_FAST_DEADLINE_US (20U)

/* 硬件触发：HDMA 通道 = 总线号；arm 时等待总线空闲 / FIFO 复位的上限 [ms] */
#define SPI_TRIG_IDLE_TIMEOUT_MS (2U)

/**
 * @brief SPI 实例上下文
 */
typedef struct {
    SPI_Type    *base;       /**< SPI 寄存器基地址 */
    clock_name_t clock;      /**< 外设时钟 */
    uint8_t      cs_en;      /**< CS_EN 编码值（spi_cs_index_t） */
    uint8_t      data_bits;  /**< 每帧位数（init 时固定） */
    bool         initialized; /**< 是否已初始化 */
    bool         trig_armed;  /**< 硬件触发单帧已启用（transfer 禁用） */
    uint8_t      trig_tx_depth; /**< TX FIFO 深度 [word]（arm 时读 CONFIG） */
    uint32_t     trig_tx_word;  /**< 每帧发送字（补满 TX FIFO 用） */
} spi_ctx_t;

static spi_ctx_t s_spi_ctx[SPI_INSTANCE_COUNT] = {
    { .base = HPM_SPI0, .clock = clock_spi0 },
    { .base = HPM_SPI1, .clock = clock_spi1 },
    { .base = HPM_SPI2, .clock = clock_spi2 },
    { .base = HPM_SPI3, .clock = clock_spi3 },
};

/* 硬件触发 DMA 源字：写入 SPI CMD 的值（命令段关闭时为哑元 0xFF，仅用于启动传输）。
 * 放在 .noncacheable（DLM）：DMA 直接读取，无 D-Cache 一致性问题。 */
static volatile uint32_t s_spi_trig_cmd_word __attribute__((section(".noncacheable.bss")));

/* 硬件触发观测（Ozone 直读）：
 *   stale_drop   = fetch 时 RX FIFO 多于 1 帧而丢弃的旧帧数（= 消费点漏拍次数）
 *   busy_timeout = fetch 时帧仍在传输且等待超时的次数
 *   wait_max     = fetch 内等待帧完成的最长耗时 [cycle]（正常应为 0） */
volatile uint32_t g_spi_trig_stale_drop __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_spi_trig_busy_timeout __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_spi_trig_wait_max __attribute__((section(".noncacheable.bss")));

/**
 * @brief 读取 mcycle（内联，避免热路径函数调用）
 * @return mcycle 低 32 位
 */
static inline __attribute__((always_inline)) uint32_t spi_read_cycle(void)
{
    uint32_t value;
    __asm__ volatile("csrr %0, mcycle" : "=r"(value));
    return value;
}

/* timeout_ms 语义：0 = 不等待；UINT32_MAX = 无限等待；其他 = 毫秒超时 */
/**
 * @brief 毫秒转 CPU cycle
 * @param ms 毫秒数
 * @return 对应 cycle 数
 */
static inline uint32_t spi_ms_to_cycles(uint32_t ms)
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
static inline bool spi_timeout_elapsed(uint32_t start, uint32_t timeout_cycles,
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
 * 实现（按总线实例）
 * ============================================================================ */

/*
 * 快速单帧传输：TRANSCTRL/CS_EN 已由 init 预置，此处不再做每帧的
 * FIFO/控制器复位（SDK spi_transfer 的该开销在 25kHz 采样下不可接受）。
 * 流程：等空闲 -> 写 CMD（触发，命令相位关闭时也必须写）-> 写 DATA
 *       -> 等 RX -> 读 DATA -> 等 CS 释放。
 * 依据：SDK 文档 "the command value must be set before transmission"；
 *       IP 文档 "SPIActive becomes 1 after the SPI command register is written"。
 */
/**
 * @brief 快速单帧传输（预置 TRANSCTRL/CS_EN，免去每帧复位开销）
 * @param ctx 实例上下文
 * @param tx 发送缓冲
 * @param rx 接收缓冲
 * @param data_bytes 数据字节数
 * @return 0 = 成功；-1 = 超时
 */
static int spi_frame_fast(spi_ctx_t *ctx, const uint8_t *tx, uint8_t *rx, uint8_t data_bytes)
{
    SPI_Type *base = ctx->base;
    uint32_t word = 0U;
    uint32_t retry;
    uint32_t start = intf_clock_get_cycle();
    uint32_t budget =
        (uint32_t)(((uint64_t)intf_clock_get_cpu_freq() / 1000000U) * SPI_FAST_DEADLINE_US);

#define SPI_FAST_EXPIRED() ((uint32_t)(intf_clock_get_cycle() - start) > budget)

    /* 1) 等待总线空闲（上一次传输完成、CS 已释放） */
    retry = 0U;
    while (spi_is_active(base)) {
        if ((++retry > SPI_FAST_RETRY_MAX) || SPI_FAST_EXPIRED()) {
            return -1;
        }
    }

    /* 2) 写 CMD 触发传输（cmd_enable=false → 哑元 0xFF） */
    base->CMD = SPI_CMD_CMD_SET(0xff);

    /* 3) 组装并写入 TX 数据 */
    for (uint8_t i = 0U; i < data_bytes; i++) {
        word |= (uint32_t) tx[i] << (8U * i);
    }
    base->DATA = word;

    /* 4) 等待 RX 数据就绪 */
    retry = 0U;
    while ((base->STATUS & SPI_STATUS_RXEMPTY_MASK) != 0U) {
        if ((++retry > SPI_FAST_RETRY_MAX) || SPI_FAST_EXPIRED()) {
            return -1;
        }
    }
    word = base->DATA;
    for (uint8_t i = 0U; i < data_bytes; i++) {
        rx[i] = (uint8_t) (word >> (8U * i));
    }

    /* 5) 等待传输结束（SPIACTIVE 清零 = CS 释放，保证帧间隔 Tpause） */
    retry = 0U;
    while (spi_is_active(base)) {
        if ((++retry > SPI_FAST_RETRY_MAX) || SPI_FAST_EXPIRED()) {
            return -1;
        }
    }

#undef SPI_FAST_EXPIRED
    return 0;
}

/**
 * @brief 初始化指定 SPI 总线
 * @param bus 总线号
 * @param cfg SPI 配置
 * @return 0 = 成功；-1 = 参数非法或分频失败
 */
static int spi_init_impl(uint8_t bus, const intf_spi_cfg_t *cfg)
{
    spi_timing_config_t timing = { 0 };
    spi_format_config_t format = { 0 };
    spi_ctx_t *ctx;

    if ((bus >= SPI_INSTANCE_COUNT) || (cfg == NULL) || (cfg->sclk_hz == 0U) ||
        (cfg->data_bits == 0U) || (cfg->data_bits > 32U) || (cfg->cs_index > 3U)) {
        return -1;
    }

    ctx = &s_spi_ctx[bus];

    clock_add_to_group(ctx->clock, 0);

    spi_master_get_default_timing_config(&timing);
    timing.master_config.clk_src_freq_in_hz = clock_get_frequency(ctx->clock);
    timing.master_config.sclk_freq_in_hz = cfg->sclk_hz;
    if (spi_master_timing_init(ctx->base, &timing) != status_success) {
        return -1; /* 源频无法整数分频到目标 sclk */
    }

    spi_master_get_default_format_config(&format);
    format.common_config.data_len_in_bits = cfg->data_bits;
    format.common_config.mode = spi_master_mode;
    format.common_config.cpol = (cfg->cpol != 0U) ? spi_sclk_high_idle : spi_sclk_low_idle;
    format.common_config.cpha = (cfg->cpha != 0U) ? spi_sclk_sampling_even_clk_edges
                                                  : spi_sclk_sampling_odd_clk_edges;
    spi_format_init(ctx->base, &format);

    /* cs_index 0..3 -> spi_cs_index_t 编码（1/2/4/8） */
    ctx->cs_en = (uint8_t)(1U << cfg->cs_index);
    ctx->data_bits = cfg->data_bits;

    /*
     * 预置传输控制（trans_mode / 帧计数 / CS_EN），供快速单帧路径复用。
     * spi_control_init 内部会复位 FIFO/控制器——仅此一次，不在每帧重复。
     */
    {
        spi_control_config_t ctrl = { 0 };

        spi_master_get_default_control_config(&ctrl);
        ctrl.common_config.trans_mode = spi_trans_write_read_together;
        ctrl.common_config.data_phase_fmt = spi_single_io_mode;
#if defined(HPM_IP_FEATURE_SPI_CS_SELECT) && (HPM_IP_FEATURE_SPI_CS_SELECT == 1)
        ctrl.common_config.cs_index = ctx->cs_en;
#endif
        if (spi_control_init(ctx->base, &ctrl, 1U, 1U) != status_success) {
            return -1;
        }
    }

    ctx->initialized = true;

    return 0;
}

/**
 * @brief SPI 传输（单帧走快速路径，否则 SDK 路径并按时序重试）
 * @param bus 总线号
 * @param tx 发送缓冲
 * @param rx 接收缓冲
 * @param frames 帧数
 * @param timeout_ms 超时毫秒语义
 * @return 0 = 成功；-1 = 参数非法、未初始化或超时
 */
static int spi_transfer_impl(uint8_t bus, const void *tx, void *rx,
                             size_t frames, uint32_t timeout_ms)
{
    spi_control_config_t ctrl = { 0 };
    spi_ctx_t *ctx;
    uint32_t start;
    uint32_t timeout_cycles;

    if ((bus >= SPI_INSTANCE_COUNT) || (tx == NULL) || (rx == NULL) || (frames == 0U)) {
        return -1;
    }

    ctx = &s_spi_ctx[bus];
    if (!ctx->initialized || ctx->trig_armed) {
        return -1; /* 硬件触发已接管总线（单一所有者） */
    }

    /* 单帧（编码器场景）：快速路径，省去 SDK 每帧 FIFO/控制器复位开销。
     * 失败直接返回（不再回退 SDK 路径）：SDK 路径按 timeout_ms 阻塞，而本调用
     * 位于 25kHz ADC 中断内，毫秒级阻塞会饿死主循环；失败样本由上层
     * "采样保持 + 错误计数"消化。 */
    if (frames == 1U) {
        uint8_t data_bytes = (uint8_t) ((ctx->data_bits + 7U) / 8U);

        return spi_frame_fast(ctx, (const uint8_t *) tx, (uint8_t *) rx, data_bytes);
    }

    spi_master_get_default_control_config(&ctrl);
    ctrl.common_config.trans_mode = spi_trans_write_read_together;
    ctrl.common_config.data_phase_fmt = spi_single_io_mode;
#if defined(HPM_IP_FEATURE_SPI_CS_SELECT) && (HPM_IP_FEATURE_SPI_CS_SELECT == 1)
    ctrl.common_config.cs_index = ctx->cs_en;
#endif

    start = intf_clock_get_cycle();
    timeout_cycles = spi_ms_to_cycles(timeout_ms);

    for (;;) {
        hpm_stat_t status = spi_transfer(ctx->base, &ctrl, NULL, NULL,
                                         (uint8_t *) tx, (uint32_t) frames,
                                         (uint8_t *) rx, (uint32_t) frames);

        if (status == status_success) {
            return 0;
        }
        if (status != status_spi_master_busy) {
            return -1; /* 参数错误 / 内部重试超时等 */
        }
        if (spi_timeout_elapsed(start, timeout_cycles, timeout_ms)) {
            return -1;
        }
    }
}

/* ============================================================================
 * 硬件触发单帧
 * ============================================================================ */

/**
 * @brief 等待总线空闲（有界）
 * @param base SPI 基地址
 * @return true = 已空闲；false = 超时
 */
static bool spi_wait_idle(SPI_Type *base)
{
    uint32_t start = intf_clock_get_cycle();
    uint32_t budget = spi_ms_to_cycles(SPI_TRIG_IDLE_TIMEOUT_MS);

    while (spi_is_active(base)) {
        if ((uint32_t)(intf_clock_get_cycle() - start) >= budget) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 复位 TX/RX FIFO（有界等待自动清零）
 * @param base SPI 基地址
 */
static void spi_reset_fifos(SPI_Type *base)
{
    uint32_t start = intf_clock_get_cycle();
    uint32_t budget = spi_ms_to_cycles(SPI_TRIG_IDLE_TIMEOUT_MS);

    base->CTRL |= (SPI_CTRL_TXFIFORST_MASK | SPI_CTRL_RXFIFORST_MASK);
    while ((base->CTRL & (SPI_CTRL_TXFIFORST_MASK | SPI_CTRL_RXFIFORST_MASK)) != 0U) {
        if ((uint32_t)(intf_clock_get_cycle() - start) >= budget) {
            break;
        }
    }
}

/**
 * @brief 启用硬件触发单帧（触发 → DMA 写 CMD）
 * @param bus 总线号
 * @param cfg 触发配置
 * @return 0 = 成功；-1 = 参数非法/未初始化/总线忙/DMA 配置失败
 */
static int spi_trig_arm_impl(uint8_t bus, const intf_spi_trig_cfg_t *cfg)
{
    spi_ctx_t *ctx;
    SPI_Type *base;
    dma_channel_config_t dma_cfg;
    uint8_t dma_ch = bus;
    uint8_t mux_src;

    if ((bus >= SPI_INSTANCE_COUNT) || (cfg == NULL)) {
        return -1;
    }
    ctx = &s_spi_ctx[bus];
    if (!ctx->initialized) {
        return -1;
    }
    if (ctx->trig_armed) {
        return 0; /* 幂等 */
    }
    switch (cfg->src) {
    case INTF_SPI_TRIG_DMA_REQ0: mux_src = (uint8_t)HPM_DMA_SRC_MOT_0; break;
    case INTF_SPI_TRIG_DMA_REQ1: mux_src = (uint8_t)HPM_DMA_SRC_MOT_1; break;
    default: return -1;
    }

    base = ctx->base;
    if (!spi_wait_idle(base)) {
        return -1;
    }

    /* 1) FIFO 清空 + TX 预填满（每次触发消耗 1 字；TX 空时传输暂停而非乱发） */
    spi_reset_fifos(base);
    ctx->trig_tx_word = cfg->tx_word;
    ctx->trig_tx_depth = spi_get_tx_fifo_size(base);
    for (uint8_t i = 0U; i < ctx->trig_tx_depth; i++) {
        base->DATA = cfg->tx_word;
    }

    /* 2) HDMA：触发请求到达 → 搬 1 字（0xFF）到 SPI CMD；无限循环、源/目的地址固定 */
    s_spi_trig_cmd_word = SPI_CMD_CMD_SET(0xFFU);
    dma_disable_channel(HPM_HDMA, dma_ch);
    dma_default_channel_config(HPM_HDMA, &dma_cfg);
    dma_cfg.src_addr = core_local_mem_to_sys_address(0U, (uint32_t)&s_spi_trig_cmd_word);
    dma_cfg.dst_addr = (uint32_t)&base->CMD;
    dma_cfg.src_width = DMA_TRANSFER_WIDTH_WORD;
    dma_cfg.dst_width = DMA_TRANSFER_WIDTH_WORD;
    dma_cfg.src_addr_ctrl = DMA_ADDRESS_CONTROL_FIXED;
    dma_cfg.dst_addr_ctrl = DMA_ADDRESS_CONTROL_FIXED;
    dma_cfg.size_in_byte = sizeof(uint32_t);
    dma_cfg.src_mode = DMA_HANDSHAKE_MODE_NORMAL;
    dma_cfg.dst_mode = DMA_HANDSHAKE_MODE_HANDSHAKE;
    dma_cfg.en_infiniteloop = true;
    dma_cfg.handshake_opt = DMA_HANDSHAKE_OPT_ALL_TRANSIZE;
    dma_cfg.priority = DMA_CHANNEL_PRIORITY_HIGH;
    dma_cfg.interrupt_mask = DMA_INTERRUPT_MASK_ALL; /* 纯硬件链路：不产生 DMA 中断 */
    if (dma_setup_channel(HPM_HDMA, dma_ch, &dma_cfg, true) != status_success) {
        return -1;
    }
    dmamux_config(HPM_DMAMUX, DMA_SOC_CHN_TO_DMAMUX_CHN(HPM_HDMA, dma_ch), mux_src, true);

    g_spi_trig_stale_drop = 0U;
    g_spi_trig_busy_timeout = 0U;
    g_spi_trig_wait_max = 0U;
    ctx->trig_armed = true;
    return 0;
}

/**
 * @brief 取最近一次硬件触发帧（中断上下文，有界）+ 补满 TX FIFO
 * @param bus 总线号
 * @param rx_word 输出接收字
 * @param wait_cycles 帧仍在传输时的最长等待 [cycle]
 * @return 0 = 新帧；-1 = 无新帧/未 arm；-2 = 等待超时
 * @note 不变式：每次 fetch 清空 RX FIFO → 下一次 fetch 取到的必为其后触发的帧
 */
ATTR_RAMFUNC
static int spi_trig_fetch_impl(uint8_t bus, uint32_t *rx_word, uint32_t wait_cycles)
{
    spi_ctx_t *ctx = &s_spi_ctx[bus];
    SPI_Type *base = ctx->base;
    uint32_t status;
    uint32_t count;
    uint32_t word = 0U;
    int rc = 0;

    if (!ctx->trig_armed) {
        return -1;
    }

    status = base->STATUS;
    if ((status & SPI_STATUS_SPIACTIVE_MASK) != 0U) {
        uint32_t start = spi_read_cycle();
        uint32_t waited;

        do {
            status = base->STATUS;
            waited = spi_read_cycle() - start;
        } while (((status & SPI_STATUS_SPIACTIVE_MASK) != 0U) && (waited <= wait_cycles));
        if (waited > g_spi_trig_wait_max) {
            g_spi_trig_wait_max = waited;
        }
        if ((status & SPI_STATUS_SPIACTIVE_MASK) != 0U) {
            g_spi_trig_busy_timeout++;
            rc = -2; /* 本拍帧未完成：已完成的旧帧一律丢弃 */
        }
    }

    /* 排空 RX FIFO，仅保留最新一帧 */
    count = (SPI_STATUS_RXNUM_7_6_GET(status) << 6U) | SPI_STATUS_RXNUM_5_0_GET(status);
    if ((count == 0U) && (rc == 0)) {
        rc = -1; /* 本拍无触发帧（触发链/DMA 未工作） */
    }
    if (count > 1U) {
        g_spi_trig_stale_drop += (count - 1U);
    }
    while (count != 0U) {
        word = base->DATA;
        count--;
    }
    if (rc == 0) {
        *rx_word = word;
    }

    /* 补满 TX FIFO（漏拍后仍保持后续触发不欠载） */
    status = base->STATUS;
    count = (SPI_STATUS_TXNUM_7_6_GET(status) << 6U) | SPI_STATUS_TXNUM_5_0_GET(status);
    while (count < ctx->trig_tx_depth) {
        base->DATA = ctx->trig_tx_word;
        count++;
    }
    return rc;
}

/**
 * @brief 关闭硬件触发（停 DMA、等空闲、清 FIFO）
 * @param bus 总线号
 */
static void spi_trig_disarm_impl(uint8_t bus)
{
    spi_ctx_t *ctx;

    if (bus >= SPI_INSTANCE_COUNT) {
        return;
    }
    ctx = &s_spi_ctx[bus];
    if (!ctx->trig_armed) {
        return;
    }
    /* DMAMUX 在本 SoC 为只写（DMAMUX_SOC_WRITEONLY）：整字写入关闭 */
    dmamux_config(HPM_DMAMUX, DMA_SOC_CHN_TO_DMAMUX_CHN(HPM_HDMA, bus), 0U, false);
    dma_disable_channel(HPM_HDMA, bus);
    (void)spi_wait_idle(ctx->base);
    spi_reset_fifos(ctx->base);
    ctx->trig_armed = false;
}

/**
 * @brief 反初始化指定 SPI 总线
 * @param bus 总线号
 */
static void spi_deinit_impl(uint8_t bus)
{
    if (bus >= SPI_INSTANCE_COUNT) {
        return;
    }
    s_spi_ctx[bus].initialized = false;
}

/* 诊断：回读分频寄存器计算实际 SCLK（0xff = 源频未分频） */
/**
 * @brief 回读分频寄存器计算实际 SCLK
 * @param bus 总线号
 * @return 实际 SCLK [Hz]；未初始化/越界返回 0
 */
static uint32_t spi_get_sclk_hz_impl(uint8_t bus)
{
    spi_ctx_t *ctx;
    uint32_t div;
    uint32_t src;

    if (bus >= SPI_INSTANCE_COUNT) {
        return 0U;
    }

    ctx = &s_spi_ctx[bus];
    if (!ctx->initialized) {
        return 0U;
    }

    src = clock_get_frequency(ctx->clock);
    div = SPI_TIMING_SCLK_DIV_GET(ctx->base->TIMING);

    if (div == 0xffU) {
        return src;
    }
    return src / ((div + 1U) * 2U);
}

/* ============================================================================
 * 每实例设备对象（风格 A）
 * ============================================================================ */

/**
 * @brief SPI0 初始化包装
 * @param cfg SPI 配置
 * @return 0 = 成功；-1 = 失败
 */
static int spi0_init(const intf_spi_cfg_t *cfg) { return spi_init_impl(0U, cfg); }

/**
 * @brief SPI0 传输包装
 * @param tx 发送缓冲
 * @param rx 接收缓冲
 * @param frames 帧数
 * @param timeout_ms 超时毫秒语义
 * @return 0 = 成功；-1 = 失败
 */
static int spi0_transfer(const void *tx, void *rx, size_t frames, uint32_t timeout_ms)
{ return spi_transfer_impl(0U, tx, rx, frames, timeout_ms); }

/**
 * @brief SPI0 反初始化包装
 */
static void spi0_deinit(void) { spi_deinit_impl(0U); }

/**
 * @brief SPI0 读取实际 SCLK
 * @return 实际 SCLK [Hz]
 */
static uint32_t spi0_get_sclk_hz(void) { return spi_get_sclk_hz_impl(0U); }

/**
 * @brief SPI0 启用硬件触发单帧
 * @param cfg 触发配置
 * @return 0 = 成功；-1 = 失败
 */
static int spi0_trig_arm(const intf_spi_trig_cfg_t *cfg) { return spi_trig_arm_impl(0U, cfg); }

/**
 * @brief SPI0 取硬件触发帧
 * @param rx_word 输出接收字
 * @param wait_cycles 最长等待 [cycle]
 * @return 0 = 新帧；-1 = 无新帧；-2 = 超时
 */
ATTR_RAMFUNC
static int spi0_trig_fetch(uint32_t *rx_word, uint32_t wait_cycles)
{ return spi_trig_fetch_impl(0U, rx_word, wait_cycles); }

/**
 * @brief SPI0 关闭硬件触发
 */
static void spi0_trig_disarm(void) { spi_trig_disarm_impl(0U); }

/**
 * @brief SPI1 初始化包装
 * @param cfg SPI 配置
 * @return 0 = 成功；-1 = 失败
 */
static int spi1_init(const intf_spi_cfg_t *cfg) { return spi_init_impl(1U, cfg); }

/**
 * @brief SPI1 传输包装
 * @param tx 发送缓冲
 * @param rx 接收缓冲
 * @param frames 帧数
 * @param timeout_ms 超时毫秒语义
 * @return 0 = 成功；-1 = 失败
 */
static int spi1_transfer(const void *tx, void *rx, size_t frames, uint32_t timeout_ms)
{ return spi_transfer_impl(1U, tx, rx, frames, timeout_ms); }

/**
 * @brief SPI1 反初始化包装
 */
static void spi1_deinit(void) { spi_deinit_impl(1U); }

/**
 * @brief SPI1 读取实际 SCLK
 * @return 实际 SCLK [Hz]
 */
static uint32_t spi1_get_sclk_hz(void) { return spi_get_sclk_hz_impl(1U); }

/**
 * @brief SPI1 启用硬件触发单帧
 * @param cfg 触发配置
 * @return 0 = 成功；-1 = 失败
 */
static int spi1_trig_arm(const intf_spi_trig_cfg_t *cfg) { return spi_trig_arm_impl(1U, cfg); }

/**
 * @brief SPI1 取硬件触发帧
 * @param rx_word 输出接收字
 * @param wait_cycles 最长等待 [cycle]
 * @return 0 = 新帧；-1 = 无新帧；-2 = 超时
 */
ATTR_RAMFUNC
static int spi1_trig_fetch(uint32_t *rx_word, uint32_t wait_cycles)
{ return spi_trig_fetch_impl(1U, rx_word, wait_cycles); }

/**
 * @brief SPI1 关闭硬件触发
 */
static void spi1_trig_disarm(void) { spi_trig_disarm_impl(1U); }

/**
 * @brief SPI2 初始化包装
 * @param cfg SPI 配置
 * @return 0 = 成功；-1 = 失败
 */
static int spi2_init(const intf_spi_cfg_t *cfg) { return spi_init_impl(2U, cfg); }

/**
 * @brief SPI2 传输包装
 * @param tx 发送缓冲
 * @param rx 接收缓冲
 * @param frames 帧数
 * @param timeout_ms 超时毫秒语义
 * @return 0 = 成功；-1 = 失败
 */
static int spi2_transfer(const void *tx, void *rx, size_t frames, uint32_t timeout_ms)
{ return spi_transfer_impl(2U, tx, rx, frames, timeout_ms); }

/**
 * @brief SPI2 反初始化包装
 */
static void spi2_deinit(void) { spi_deinit_impl(2U); }

/**
 * @brief SPI2 读取实际 SCLK
 * @return 实际 SCLK [Hz]
 */
static uint32_t spi2_get_sclk_hz(void) { return spi_get_sclk_hz_impl(2U); }

/**
 * @brief SPI2 启用硬件触发单帧
 * @param cfg 触发配置
 * @return 0 = 成功；-1 = 失败
 */
static int spi2_trig_arm(const intf_spi_trig_cfg_t *cfg) { return spi_trig_arm_impl(2U, cfg); }

/**
 * @brief SPI2 取硬件触发帧
 * @param rx_word 输出接收字
 * @param wait_cycles 最长等待 [cycle]
 * @return 0 = 新帧；-1 = 无新帧；-2 = 超时
 */
ATTR_RAMFUNC
static int spi2_trig_fetch(uint32_t *rx_word, uint32_t wait_cycles)
{ return spi_trig_fetch_impl(2U, rx_word, wait_cycles); }

/**
 * @brief SPI2 关闭硬件触发
 */
static void spi2_trig_disarm(void) { spi_trig_disarm_impl(2U); }

/**
 * @brief SPI3 初始化包装
 * @param cfg SPI 配置
 * @return 0 = 成功；-1 = 失败
 */
static int spi3_init(const intf_spi_cfg_t *cfg) { return spi_init_impl(3U, cfg); }

/**
 * @brief SPI3 传输包装
 * @param tx 发送缓冲
 * @param rx 接收缓冲
 * @param frames 帧数
 * @param timeout_ms 超时毫秒语义
 * @return 0 = 成功；-1 = 失败
 */
static int spi3_transfer(const void *tx, void *rx, size_t frames, uint32_t timeout_ms)
{ return spi_transfer_impl(3U, tx, rx, frames, timeout_ms); }

/**
 * @brief SPI3 反初始化包装
 */
static void spi3_deinit(void) { spi_deinit_impl(3U); }

/**
 * @brief SPI3 读取实际 SCLK
 * @return 实际 SCLK [Hz]
 */
static uint32_t spi3_get_sclk_hz(void) { return spi_get_sclk_hz_impl(3U); }

/**
 * @brief SPI3 启用硬件触发单帧
 * @param cfg 触发配置
 * @return 0 = 成功；-1 = 失败
 */
static int spi3_trig_arm(const intf_spi_trig_cfg_t *cfg) { return spi_trig_arm_impl(3U, cfg); }

/**
 * @brief SPI3 取硬件触发帧
 * @param rx_word 输出接收字
 * @param wait_cycles 最长等待 [cycle]
 * @return 0 = 新帧；-1 = 无新帧；-2 = 超时
 */
ATTR_RAMFUNC
static int spi3_trig_fetch(uint32_t *rx_word, uint32_t wait_cycles)
{ return spi_trig_fetch_impl(3U, rx_word, wait_cycles); }

/**
 * @brief SPI3 关闭硬件触发
 */
static void spi3_trig_disarm(void) { spi_trig_disarm_impl(3U); }

static const intf_spi_t s_spi0_dev = {
    .instance_id = 0U,
    .init = spi0_init,
    .transfer = spi0_transfer,
    .deinit = spi0_deinit,
    .get_sclk_hz = spi0_get_sclk_hz,
    .trig_arm = spi0_trig_arm,
    .trig_fetch = spi0_trig_fetch,
    .trig_disarm = spi0_trig_disarm,
};

static const intf_spi_t s_spi1_dev = {
    .instance_id = 1U,
    .init = spi1_init,
    .transfer = spi1_transfer,
    .deinit = spi1_deinit,
    .get_sclk_hz = spi1_get_sclk_hz,
    .trig_arm = spi1_trig_arm,
    .trig_fetch = spi1_trig_fetch,
    .trig_disarm = spi1_trig_disarm,
};

static const intf_spi_t s_spi2_dev = {
    .instance_id = 2U,
    .init = spi2_init,
    .transfer = spi2_transfer,
    .deinit = spi2_deinit,
    .get_sclk_hz = spi2_get_sclk_hz,
    .trig_arm = spi2_trig_arm,
    .trig_fetch = spi2_trig_fetch,
    .trig_disarm = spi2_trig_disarm,
};

static const intf_spi_t s_spi3_dev = {
    .instance_id = 3U,
    .init = spi3_init,
    .transfer = spi3_transfer,
    .deinit = spi3_deinit,
    .get_sclk_hz = spi3_get_sclk_hz,
    .trig_arm = spi3_trig_arm,
    .trig_fetch = spi3_trig_fetch,
    .trig_disarm = spi3_trig_disarm,
};

/* ============================================================================
 * 注册
 * ============================================================================ */

void hpm_spi_driver_register(void)
{
    intf_spi_register(&s_spi0_dev);
    intf_spi_register(&s_spi1_dev);
    intf_spi_register(&s_spi2_dev);
    intf_spi_register(&s_spi3_dev);
}
