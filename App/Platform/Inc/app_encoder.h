/**
 * @file    app_encoder.h
 * @brief   编码器平台封装（双 KTH7823）
 * @author  Kaiser
 *
 * 板级映射：
 *   APP_ENCODER_ROTOR  -> SPI3（PA10-13），转子 1:1
 *   APP_ENCODER_OUTPUT -> SPI1（PA26-29），出轴 49:50（游标）
 *
 * 实时性：read_raw 为阻塞短操作（标称 ~5µs），无打印/动态分配；
 *         每实例单所有者，不可在多上下文并发调用。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_ENCODER_H
#define APP_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 编码器实例标识
 */
typedef enum {
    APP_ENCODER_ROTOR = 0, /**< 转子编码器：SPI3 */
    APP_ENCODER_OUTPUT,    /**< 出轴编码器：SPI1 */
    APP_ENCODER_COUNT
} app_encoder_id_t;

/**
 * @brief 注册编码器驱动并初始化双路（KTH7823，mode3，10MHz）。
 * @return 0 = 全部成功；-1 = 存在失败（可查错误计数/寄存器读进一步诊断）
 */
int app_encoder_init(void);

/**
 * @brief 读取单圈绝对位置原始值（16bit 原码，未修正）。
 * @return 0 成功，-1 失败（累计于 get_error_count）
 * @note 转子实例在采样器已声明所有权后（app_encoder_sampler_claim）不再触碰物理 SPI，
 *       改返回一致快照（无 I/O）。出轴实例始终走物理 SPI。
 */
int app_encoder_read_raw(app_encoder_id_t id, uint16_t* raw);

/**
 * @brief 转子编码器一致快照（读者可见）
 */
typedef struct {
    uint16_t raw;              /**< 最近被接受的原始值（坏帧不写入） */
    float    rad;              /**< 机械角（未加软件零点）[rad]，按器件分辨率换算 */
    uint32_t seq;              /**< 接受样本计数（仅接受时 +1） */
    uint32_t timestamp_cycles; /**< 最近接受样本时刻 [cycle] */
    uint32_t age_cycles;       /**< 读取时计算的年龄 [cycle] */
    uint16_t consecutive_fail; /**< 连续失败/跳变计数 */
    bool     valid;            /**< 快照有效 */
    bool     jumped;           /**< 最近样本因跳变被拒 */
    bool     read_failed;      /**< 最近设备读失败 */
} app_encoder_rotor_snapshot_t;

/**
 * @brief 声明转子 SPI3 的运行期所有权归采样器 ISR（此后运行期读走快照）。
 *        应在启动 GPTMR 采样器之前调用；启动初始化/自检阶段不调用。
 */
void app_encoder_sampler_claim(void);

/**
 * @brief 回滚转子 SPI3 采样器所有权声明（启动失败恢复路径）。
 */
void app_encoder_sampler_release_claim(void);

/**
 * @brief 采样器是否已声明所有权
 */
bool app_encoder_sampler_active(void);

/**
 * @brief 启动 GPTMR1 CH3 @12.5kHz 转子采样器（声明所有权 + 注册回调 + 启动）。
 * @return 0 = 成功；-1 = 编码器未初始化/通道配置失败
 * @note PLIC：GPTMR 优先级 3 > ADC0=2，采样 ISR 可抢占 25kHz FOC ISR。
 */
int app_encoder_sampler_start(void);

/**
 * @brief 转子编码器采样一次（在采样器 ISR 内调用）：SPI 读 + 快照策略。
 * @param now_cycles 本拍时刻 [cycle]（用于样本时间戳）
 * @return 0 = 接受；1 = 跳变保持；-1 = 读失败/连续失败
 */
int app_encoder_sample_rotor_at(uint32_t now_cycles);

/**
 * @brief 记录当前时刻的转子采样（兼容包装，内部取 intf_clock_get_cycle）
 */
int app_encoder_sample_rotor(void);

/**
 * @brief 读取转子一致快照（无 I/O、有界重试，绝不长时间自旋）
 * @param out 输出快照
 * @return 0 = 参数合法（快照是否有效看 out->valid）；-1 = 参数错误
 */
int app_encoder_get_rotor_snapshot(app_encoder_rotor_snapshot_t* out);

/**
 * @brief 读取转子一致快照（ISR 版本：最多 1 次重试，绝不长时间自旋）
 * @param out 输出快照
 * @return 0 = 参数合法；-1 = 参数错误
 * @note 供优先级高于采样写者的 ISR（如 ADC0 FOC）使用。
 */
int app_encoder_read_rotor_isr(app_encoder_rotor_snapshot_t* out);

/**
 * @brief 转子编码器"角度跳变（坏帧）"计数
 * @return 累计次数（SPI 帧无 CRC，超阈值样本被丢弃并计入）
 */
uint32_t app_encoder_get_rotor_jump_count(void);

/**
 * @brief 读取转子共享采样缓存并换算为机械角（按器件分辨率；无 I/O）。
 * @param rad 输出机械角（未加软件零点）[rad]，范围 [0, 2π)
 * @param valid 输出缓存有效性
 * @param seq 输出采样序号（每次接受 +1；可为 NULL）
 * @return 0 = 成功；-1 = 参数错误
 */
int app_encoder_get_rotor_rad(float* rad, bool* valid, uint32_t* seq);

/**
 * @brief 读取转子共享采样缓存（无 I/O）。
 * @param raw 输出原始值（未加软件零点）
 * @param valid 输出缓存有效性
 * @param seq 输出采样序号（每次接受 +1；可为 NULL）。
 *            消费方用 seq 判新样本（12.5kHz 采样可被 25kHz 环复用）。
 * @return 0 = 成功；-1 = 参数错误
 * @note 运行期 SPI3 由采样器 ISR 独占（app_encoder_sampler_claim）；消费方只读快照。
 */
int app_encoder_get_rotor_raw(uint16_t* raw, bool* valid, uint32_t* seq);

/**
 * @brief 读取零点修正后的单圈位置：(raw − zero) & 0xFFFF。
 * @return 0 成功，-1 失败
 */
int app_encoder_read_position(app_encoder_id_t id, uint16_t* pos);

/**
 * @brief 读取机械角，单位 rad，范围 [0, 2π)。
 * @return 0 成功，-1 失败
 */
int app_encoder_read_rad(app_encoder_id_t id, float* rad);

/**
 * @brief 读取机械角，单位 deg，范围 [0, 360)。
 * @return 0 成功，-1 失败
 */
int app_encoder_read_deg(app_encoder_id_t id, float* deg);

/**
 * @brief 读寄存器（诊断；如 0x09 = RD，出厂默认 1）。
 * @return 0 成功，-1 失败
 */
int app_encoder_read_reg(app_encoder_id_t id, uint8_t addr, uint8_t* val);

/*
 * 零点（软件方案，推荐）：
 *   记录当前原始值到 flash 参数区（app_param），掉电保持，
 *   不消耗编码器 MTP（Z 寄存器寿命仅 1000 次写）。
 */

/**
 * @brief 软件设置零点：记录当前原始值并保存到 flash。
 *        之后 read_position / read_rad / read_deg 均以此为基准。
 * @return 0 成功，-1 失败
 */
int app_encoder_set_zero(app_encoder_id_t id);

/**
 * @brief 清除软件零点（偏移归零并写回 flash）。
 * @return 0 成功，-1 失败
 */
int app_encoder_clear_zero(app_encoder_id_t id);

/**
 * @brief 读取当前软件零点偏移。
 * @return 0 成功，-1 失败
 */
int app_encoder_get_zero(app_encoder_id_t id, uint16_t* zero);

/**
 * @brief 是否已从 flash 加载到有效的编码器参数记录（false = 使用默认值）。
 */
bool app_encoder_is_param_loaded(void);

/*
 * 硬件零点（写编码器 MTP，器件寿命 1000 次）——仅产线一次性标定用，慎调。
 */

/**
 * @brief 写编码器 Z(15:0) 寄存器（0x00/0x01，两次 MTP 写）。
 * @return 0 成功，-1 失败
 */
int app_encoder_set_zero_mtp(app_encoder_id_t id, uint16_t zero);

/**
 * @brief 写旋转方向 RD（0x09，一次 MTP 写）；true = 顺时针角度增加（出厂默认）。
 * @return 0 成功，-1 失败
 */
int app_encoder_set_direction(app_encoder_id_t id, bool cw_increasing);

/**
 * @brief 累计传输失败次数（单调递增）。
 */
uint32_t app_encoder_get_error_count(app_encoder_id_t id);

/**
 * @brief 当前实际 SCLK（Hz），0 = 未初始化。
 */
uint32_t app_encoder_get_sclk_hz(app_encoder_id_t id);

/*
 * 采样器观测（.noncacheable.bss；Ozone/RTT 直读）：
 *   - isr_cycles：GPTMR 采样 ISR 单拍耗时 [cycle]（含 SPI 读 + 快照发布）
 *   - isr_cycles_max：历史最大
 *   - sample_count：采样次数
 *   - read_fail_count：设备读失败次数（含 SPI 超时）
 * PLIC 优先级：GPTMR=3 > ADC0=2（见 drv_gptmr.c / drv_adc.c），采样 ISR 可抢占 FOC ISR。
 */
extern volatile uint32_t g_encoder_isr_cycles;
extern volatile uint32_t g_encoder_isr_cycles_max;
extern volatile uint32_t g_encoder_sample_count;
extern volatile uint32_t g_encoder_read_fail_count;

#ifdef __cplusplus
}
#endif

#endif /* APP_ENCODER_H */
