/**
 * @file    app_debug_encoder.h
 * @brief   编码器自检 + 25kHz 采样仿真（双 KTH7823）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_DEBUG_ENCODER_H
#define APP_DEBUG_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Ozone 观测变量（定义于 app_debug_encoder.c，.noncacheable.bss 段，启动清零）：
 * sample 每个控制周期更新（25kHz 仿真下即 25kHz 刷新）；
 * 在 Ozone 的 Watch / Plot 窗口按符号名添加即可。
 */
extern volatile uint16_t g_enc_rotor_raw;      /* 转子编码器 16bit 原始值 */
extern volatile uint16_t g_enc_output_raw;     /* 出轴编码器 16bit 原始值 */
extern volatile float g_enc_rotor_deg;         /* 转子机械角 [deg]（零点修正后），0..360 */
extern volatile float g_enc_output_deg;        /* 出轴机械角 [deg]（零点修正后），0..360 */
extern volatile uint32_t g_enc_rotor_read_us;  /* 转子单次读耗时 [us] */
extern volatile uint32_t g_enc_output_read_us; /* 出轴单次读耗时 [us] */
extern volatile uint32_t g_enc_loop_late_us;   /* 最近一次节拍迟到 [us]，0 = 未迟到 */
extern volatile int32_t g_enc_ratio_x10000;    /* 游标比值累计 ×10000（1kHz 成对采样） */

/*
 * 自检结果：被动健康（passive，不使能桥）与通电电气标定（energized）字段分离，
 * 互不覆盖；未做电气标定时 energized_ok=0、energized_not_run=1。
 */
extern volatile uint32_t g_enc_passive_ok;       /* 1 = 被动健康检查通过 */
extern volatile uint32_t g_enc_passive_spi_hz;   /* 转子实际 SCLK [Hz] */
extern volatile uint32_t g_enc_energized_ok;     /* 1 = 通电电气标定通过 */
extern volatile uint32_t g_enc_energized_not_run;/* 1 = 尚未执行电气标定 */

/**
 * @brief 被动健康自检（boot 版，不使能桥）：读转子原始值 + RD 寄存器（0x09 bit7）。
 * @return 0 = 通过；-1 = 失败；-3 = 采样器已独占 SPI3（运行期不可再物理读）
 * @note 仅在 boot、采样器 claim 之前调用。运行期请用 health_start/health_tick
 *       （不触碰 SPI，只评估采样器快照流）。
 *       通电电气标定请走 `app_motor_identify`（Debug `CAL_ENCODER` 命令）。
 */
int app_debug_encoder_passive_selftest(void);

/* ---- 运行期健康自检（不触碰 SPI3；评估采样器快照流 100ms） ---- */
#define APP_ENC_HEALTH_TICKS (100U)         /**< 窗口 tick 数（1kHz → 100ms） */
#define APP_ENC_HEALTH_EXPECTED_SEQ (1250U) /**< 12.5kHz×100ms 名义样本数 */

extern volatile uint32_t g_enc_runtime_active;   /**< 1 = 进行中 */
extern volatile uint32_t g_enc_runtime_ok;        /**< 1 = 窗口结束且通过 */
extern volatile uint32_t g_enc_runtime_progress;  /**< 0..100 */
extern volatile uint32_t g_enc_runtime_seq_delta; /**< 窗口内接受样本增量 */
extern volatile uint32_t g_enc_runtime_valid_pct; /**< 有效快照占比 0..100 */
extern volatile uint32_t g_enc_runtime_read_fail_delta; /**< 读失败增量 */
extern volatile uint32_t g_enc_runtime_jump_delta;      /**< 跳变增量 */
extern volatile uint32_t g_enc_runtime_err_delta;       /**< 传输错误增量 */
extern volatile uint32_t g_enc_runtime_age_max_us;      /**< 窗口内最大快照年龄 [µs] */

/**
 * @brief 启动运行期健康自检（非阻塞；读取快照，不做 SPI 寄存器读）。
 * @note 若已在运行则忽略。
 */
void app_debug_encoder_health_start(void);

/**
 * @brief 运行期健康自检推进（主循环 1kHz 调用；非活动时为空操作）。
 * @note 通过判据（初步，需台架复核）：样本增量 ≥ 80% 名义值、有效占比 ≥ 99%、
 *       读失败/跳变/传输错误增量均为 0。
 */
void app_debug_encoder_health_tick(void);

/** @brief 运行期健康自检是否进行中 */
bool app_debug_encoder_health_active(void);

/**
 * @brief 初始化编码器自检：注册驱动 + 初始化双路 + 在线/寄存器检查 + 耗时实测。
 */
void app_debug_encoder_init(void);

/**
 * @brief 每控制周期调用：双路采样 + 更新观测变量 + 计时统计。
 */
void app_debug_encoder_sample(void);

/**
 * @brief 主循环节拍迟到反馈（迟到周期数，由节拍点在错过期限时调用）。
 */
void app_debug_encoder_note_loop_late(uint32_t late_cycles);

/**
 * @brief 1Hz 汇总打印（读耗时 avg/max、实际速率、迟到统计、错误计数）。
 */
void app_debug_encoder_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_DEBUG_ENCODER_H */
