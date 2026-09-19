/*
 * Debug Encoder - 编码器自检 + 25kHz 采样仿真（双 KTH7823）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 测试内容：
 *   1) 初始化双路（SPI3=转子 / SPI1=出轴，mode3，10MHz）
 *   2) 读 RD 寄存器（0x09 bit7）期望 1 → 验证寄存器读通路与器件在线
 *   3) 每控制周期采样转子（25kHz）；出轴降采样（默认 1kHz，游标差值变化率仅 1/50）
 *   4) 统计单次读耗时（avg/max）与节拍迟到 → 1Hz 汇总打印
 *
 * 说明：
 *   - 不写任何寄存器（MTP 寿命 1000 次）
 *   - 转子编码器 1:1、出轴编码器 49:50；手动转动转子即可观察两路变化
 *   - 心跳打印所在轮次不参与迟到统计（打印耗时大，避免污染节拍指标）
 */

#include "app_debug_encoder.h"

#include "app_debug_rtt.h"
#include "app_encoder.h"
#include "intf_clock.h"

#define ENC_REG_RD (0x09U)
#define ENC_REG_RD_BIT (0x80U) /* RD 位于 0x09 的 bit7，出厂默认 1 */

/* 出轴编码器降采样：游标差值变化率仅为转子的 1/50，1kHz 足够。
   设为 1 = 每周期读（双路 25kHz 压力模式）。 */
#define ENC_OUTPUT_SAMPLE_DIV (25U)

/* init 基准：read_raw 平均耗时测量次数 */
#define ENC_BENCH_ITERS (100U)

/* ============================================================================
 * Ozone 观测变量（.noncacheable.bss：启动清零 + 调试器直读，不受 D-Cache 影响）
 * sample 每个控制周期更新（25kHz 仿真下即 25kHz 刷新）
 * ============================================================================ */

volatile uint16_t g_enc_rotor_raw __attribute__((section(".noncacheable.bss")));
volatile uint16_t g_enc_output_raw __attribute__((section(".noncacheable.bss")));
volatile float    g_enc_rotor_deg __attribute__((section(".noncacheable.bss")));
volatile float    g_enc_output_deg __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_enc_rotor_read_us __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_enc_output_read_us __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_enc_loop_late_us __attribute__((section(".noncacheable.bss")));

/* ============================================================================
 * 统计窗口（print_stats 后清零）
 * ============================================================================ */

static uint32_t s_read_cycles_sum[APP_ENCODER_COUNT];
static uint32_t s_read_cycles_max[APP_ENCODER_COUNT];
static uint32_t s_read_count[APP_ENCODER_COUNT];
static uint32_t s_loop_count;
static uint32_t s_loop_late_count;
static uint32_t s_loop_late_max_us;
static uint32_t s_cpu_mhz;
static uint32_t s_sample_index;

static inline uint32_t cycles_to_us(uint32_t cycles)
{
    if (s_cpu_mhz == 0U) {
        s_cpu_mhz = intf_clock_get_cpu_freq() / 1000000U;
    }
    return (s_cpu_mhz != 0U) ? (cycles / s_cpu_mhz) : cycles;
}

static const char *encoder_name(app_encoder_id_t id)
{
    return (id == APP_ENCODER_ROTOR) ? "rotor" : "output";
}

/* 读一次 + 计时 + 更新观测变量；返回 read_raw 结果 */
static int encoder_sample_one(app_encoder_id_t id)
{
    uint32_t c0 = intf_clock_get_cycle();
    uint16_t raw = 0U;
    uint32_t mdeg = 0U;
    int ret = app_encoder_read_raw(id, &raw);
    uint32_t cycles = intf_clock_get_cycle() - c0;

    s_read_cycles_sum[id] += cycles;
    if (cycles > s_read_cycles_max[id]) {
        s_read_cycles_max[id] = cycles;
    }
    s_read_count[id]++;

    if (id == APP_ENCODER_ROTOR) {
        g_enc_rotor_read_us = cycles_to_us(cycles);
    } else {
        g_enc_output_read_us = cycles_to_us(cycles);
    }

    if (ret == 0) {
        /* 16bit 原码 → 毫度（整数运算，无浮点打印依赖） */
        mdeg = (uint32_t)(((uint64_t) raw * 360000U) / 65536U);
    }

    if (id == APP_ENCODER_ROTOR) {
        g_enc_rotor_raw = raw;
        g_enc_rotor_deg = (float) mdeg / 1000.0f;
    } else {
        g_enc_output_raw = raw;
        g_enc_output_deg = (float) mdeg / 1000.0f;
    }

    return ret;
}

/* ============================================================================
 * 对外接口
 * ============================================================================ */

void app_debug_encoder_init(void)
{
    uint16_t raw = 0U;
    int ret;

    app_debug_printf("\r\n[ENC] dual KTH7823 self-test: rotor=SPI3, output=SPI1, mode3\r\n");

    ret = app_encoder_init();
    app_debug_printf("[ENC] init: %s | sclk: rotor=%u Hz, output=%u Hz\r\n",
                     (ret == 0) ? "OK" : "FAILED",
                     (unsigned) app_encoder_get_sclk_hz(APP_ENCODER_ROTOR),
                     (unsigned) app_encoder_get_sclk_hz(APP_ENCODER_OUTPUT));

    /* 寄存器读通路 + 器件在线检查：RD（0x09 bit7）出厂默认 1 */
    for (uint8_t i = 0U; i < (uint8_t) APP_ENCODER_COUNT; i++) {
        uint8_t rd = 0U;
        app_encoder_id_t id = (app_encoder_id_t) i;

        ret = app_encoder_read_reg(id, ENC_REG_RD, &rd);
        app_debug_printf("[ENC] %-6s RD(0x09): ret=%d val=0x%02X RD=%u %s\r\n",
                         encoder_name(id), ret, (unsigned) rd,
                         (unsigned) (((rd & ENC_REG_RD_BIT) != 0U) ? 1U : 0U),
                         ((ret == 0) && ((rd & ENC_REG_RD_BIT) != 0U)) ? "OK" : "CHECK");
    }

    /* 基准：read_raw 平均耗时（实时性数据） */
    {
        uint32_t c0, cycles;

        c0 = intf_clock_get_cycle();
        for (uint32_t i = 0U; i < ENC_BENCH_ITERS; i++) {
            (void) app_encoder_read_raw(APP_ENCODER_ROTOR, &raw);
        }
        cycles = intf_clock_get_cycle() - c0;

        app_debug_printf("[ENC] read_raw bench: avg=%u cycles (~%u us) x%u @%u MHz\r\n",
                         (unsigned) (cycles / ENC_BENCH_ITERS),
                         (unsigned) cycles_to_us(cycles / ENC_BENCH_ITERS),
                         (unsigned) ENC_BENCH_ITERS,
                         (unsigned) (intf_clock_get_cpu_freq() / 1000000U));
    }
}

/* 每控制周期调用：转子每周期采样；出轴按 ENC_OUTPUT_SAMPLE_DIV 降采样 */
void app_debug_encoder_sample(void)
{
    (void) encoder_sample_one(APP_ENCODER_ROTOR);

    if ((s_sample_index % ENC_OUTPUT_SAMPLE_DIV) == 0U) {
        (void) encoder_sample_one(APP_ENCODER_OUTPUT);
    }
    s_sample_index++;
    s_loop_count++;
}

/* 主循环节拍迟到反馈（迟到周期数） */
void app_debug_encoder_note_loop_late(uint32_t late_cycles)
{
    uint32_t late_us = cycles_to_us(late_cycles);

    s_loop_late_count++;
    g_enc_loop_late_us = late_us;
    if (late_us > s_loop_late_max_us) {
        s_loop_late_max_us = late_us;
    }
}

/* 1Hz 汇总打印（并清零统计窗口） */
void app_debug_encoder_print_stats(void)
{
    uint32_t avg_rotor = (s_read_count[APP_ENCODER_ROTOR] != 0U)
                             ? cycles_to_us(s_read_cycles_sum[APP_ENCODER_ROTOR] /
                                            s_read_count[APP_ENCODER_ROTOR])
                             : 0U;
    uint32_t avg_output = (s_read_count[APP_ENCODER_OUTPUT] != 0U)
                              ? cycles_to_us(s_read_cycles_sum[APP_ENCODER_OUTPUT] /
                                             s_read_count[APP_ENCODER_OUTPUT])
                              : 0U;

    /* 观测值快照（与 Ozone 变量同源） */
    app_debug_printf("[ENC] rotor  raw=0x%04X deg=%u.%03u | output raw=0x%04X deg=%u.%03u\r\n",
                     (unsigned) g_enc_rotor_raw,
                     (unsigned) ((uint32_t) (g_enc_rotor_deg * 1000.0f) / 1000U),
                     (unsigned) ((uint32_t) (g_enc_rotor_deg * 1000.0f) % 1000U),
                     (unsigned) g_enc_output_raw,
                     (unsigned) ((uint32_t) (g_enc_output_deg * 1000.0f) / 1000U),
                     (unsigned) ((uint32_t) (g_enc_output_deg * 1000.0f) % 1000U));

    app_debug_printf(
        "[ENC] rate=%u Hz late=%u(max=%u us) | read: rotor avg=%u max=%u us, output avg=%u max=%u us | err=%u/%u\r\n",
        (unsigned) s_loop_count, (unsigned) s_loop_late_count,
        (unsigned) s_loop_late_max_us, (unsigned) avg_rotor,
        (unsigned) cycles_to_us(s_read_cycles_max[APP_ENCODER_ROTOR]), (unsigned) avg_output,
        (unsigned) cycles_to_us(s_read_cycles_max[APP_ENCODER_OUTPUT]),
        (unsigned) app_encoder_get_error_count(APP_ENCODER_ROTOR),
        (unsigned) app_encoder_get_error_count(APP_ENCODER_OUTPUT));

    /* 清零窗口 */
    for (uint8_t i = 0U; i < (uint8_t) APP_ENCODER_COUNT; i++) {
        s_read_cycles_sum[i] = 0U;
        s_read_cycles_max[i] = 0U;
        s_read_count[i] = 0U;
    }
    s_loop_count = 0U;
    s_loop_late_count = 0U;
    s_loop_late_max_us = 0U;
    g_enc_loop_late_us = 0U;
}
