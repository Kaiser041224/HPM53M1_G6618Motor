/**
 * @file    intf_default.c
 * @brief   Interface 契约层默认分发实现（设备对象注册表 + 功能 API 转发）
 * @author  Kaiser
 *
 * 各驱动在初始化时调用 intf_xxx_register() 注册其实现；本文件仅保存注册表，
 * 功能 API 在 checks 之后转发到已注册实现；未注册时返回 -1（intf_synt_get_count 返回 0）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_hrpwm.h"
#include "intf_gptmr.h"
#include "intf_adc.h"
#include "intf_trgm.h"
#include "intf_uart.h"
#include "intf_usb_cdc.h"
#include "intf_gpio.h"
#include "intf_can.h"
#include "intf_synt.h"
#include "intf_spi.h"
#include "intf_encoder.h"
#include "intf_flash.h"

#include <stddef.h>

#ifndef ATTR_RAMFUNC
#define ATTR_RAMFUNC __attribute__((section(".fast")))
#endif

/* ============================================================================
 * HRPWM Interface
 * ============================================================================ */

#define HRPWM_INSTANCE_COUNT (2U)

static const intf_hrpwm_t *s_hrpwm_ops[HRPWM_INSTANCE_COUNT] = {NULL};

int intf_hrpwm_register(const intf_hrpwm_t *ops)
{
    if (ops == NULL || ops->instance_id >= HRPWM_INSTANCE_COUNT) return -1;
    s_hrpwm_ops[ops->instance_id] = ops;
    return 0;
}

/**
 * @brief 按通道号查找所属实例的 HRPWM 实现
 * @param ch 通道号
 * @return 接口实现指针；NULL = 未注册/通道越界
 */
ATTR_RAMFUNC
static const intf_hrpwm_t *hrpwm_get_ops_by_ch(intf_hrpwm_ch_t ch)
{
    /* 通道空间：0-3 = PWM0 ch0-3；4-9 = PWM1（ch4-7 及虚拟通道 8/9 = PWM1 ch0/1） */
    uint8_t inst = (ch < 4U) ? 0U : 1U;
    if (inst >= HRPWM_INSTANCE_COUNT) return NULL;
    return s_hrpwm_ops[inst];
}

int intf_hrpwm_init_pair(intf_hrpwm_ch_t ch, const intf_hrpwm_pair_cfg_t *cfg)
{
    const intf_hrpwm_t *ops = hrpwm_get_ops_by_ch(ch);
    if (ops && ops->init_pair) return ops->init_pair(ch, cfg);
    return -1;
}

ATTR_RAMFUNC
int intf_hrpwm_set_duty(intf_hrpwm_ch_t ch, float duty)
{
    const intf_hrpwm_t *ops = hrpwm_get_ops_by_ch(ch);
    if (ops && ops->set_duty) return ops->set_duty(ch, duty);
    return -1;
}

ATTR_RAMFUNC
int intf_hrpwm_set_duty_direct(intf_hrpwm_ch_t ch, float duty)
{
    const intf_hrpwm_t *ops = hrpwm_get_ops_by_ch(ch);
    if (ops && ops->set_duty_direct) return ops->set_duty_direct(ch, duty);
    return -1;
}

ATTR_RAMFUNC
int intf_hrpwm_set_duty_direct_dual(intf_hrpwm_ch_t ch_a, float duty_a,
                                     intf_hrpwm_ch_t ch_b, float duty_b)
{
    const intf_hrpwm_t *ops = hrpwm_get_ops_by_ch(ch_a);
    if (ops && ops->set_duty_direct_dual) return ops->set_duty_direct_dual(ch_a, duty_a, ch_b, duty_b);
    return -1;
}

int intf_hrpwm_set_frequency(intf_hrpwm_inst_t inst, uint32_t frequency_hz)
{
    if (inst >= HRPWM_INSTANCE_COUNT || s_hrpwm_ops[inst] == NULL) return -1;
    if (s_hrpwm_ops[inst]->set_frequency) return s_hrpwm_ops[inst]->set_frequency(frequency_hz);
    return -1;
}

int intf_hrpwm_set_jitter(intf_hrpwm_ch_t ch, uint8_t jitter_cmp)
{
    const intf_hrpwm_t *ops = hrpwm_get_ops_by_ch(ch);
    if (ops && ops->set_jitter) return ops->set_jitter(ch, jitter_cmp);
    return -1;
}

int intf_hrpwm_start(intf_hrpwm_ch_t ch)
{
    const intf_hrpwm_t *ops = hrpwm_get_ops_by_ch(ch);
    if (ops && ops->start) return ops->start(ch);
    return -1;
}

int intf_hrpwm_stop(intf_hrpwm_ch_t ch)
{
    const intf_hrpwm_t *ops = hrpwm_get_ops_by_ch(ch);
    if (ops && ops->stop) return ops->stop(ch);
    return -1;
}

int intf_hrpwm_start_counter_only(intf_hrpwm_inst_t inst)
{
    if (inst >= HRPWM_INSTANCE_COUNT || s_hrpwm_ops[inst] == NULL) return -1;
    if (s_hrpwm_ops[inst]->start_counter_only) return s_hrpwm_ops[inst]->start_counter_only();
    return -1;
}

uint32_t intf_hrpwm_get_reload(intf_hrpwm_inst_t inst)
{
    if (inst >= HRPWM_INSTANCE_COUNT || s_hrpwm_ops[inst] == NULL) return 0;
    if (s_hrpwm_ops[inst]->get_reload) return s_hrpwm_ops[inst]->get_reload();
    return 0;
}

uint32_t intf_hrpwm_get_cmp_value(intf_hrpwm_inst_t inst, uint8_t cmp_index)
{
    if (inst >= HRPWM_INSTANCE_COUNT || s_hrpwm_ops[inst] == NULL) return 0;
    if (s_hrpwm_ops[inst]->get_cmp_value) return s_hrpwm_ops[inst]->get_cmp_value(cmp_index);
    return 0;
}

ATTR_RAMFUNC
int intf_hrpwm_force_low(intf_hrpwm_ch_t ch)
{
    const intf_hrpwm_t *ops = hrpwm_get_ops_by_ch(ch);
    if (ops && ops->force_low) return ops->force_low(ch);
    return -1;
}

ATTR_RAMFUNC
int intf_hrpwm_force_release(intf_hrpwm_ch_t ch)
{
    const intf_hrpwm_t *ops = hrpwm_get_ops_by_ch(ch);
    if (ops && ops->force_release) return ops->force_release(ch);
    return -1;
}

int intf_hrpwm_config_fault(intf_hrpwm_inst_t inst, const intf_hrpwm_fault_cfg_t *cfg)
{
    if (inst >= HRPWM_INSTANCE_COUNT || s_hrpwm_ops[inst] == NULL) return -1;
    if (s_hrpwm_ops[inst]->config_fault) return s_hrpwm_ops[inst]->config_fault(cfg);
    return -1;
}

int intf_hrpwm_clear_fault(intf_hrpwm_inst_t inst)
{
    if (inst >= HRPWM_INSTANCE_COUNT || s_hrpwm_ops[inst] == NULL) return -1;
    if (s_hrpwm_ops[inst]->clear_fault) return s_hrpwm_ops[inst]->clear_fault();
    return -1;
}

int intf_hrpwm_config_reload_irq(intf_hrpwm_inst_t inst, intf_hrpwm_irq_callback_t callback)
{
    if (inst >= HRPWM_INSTANCE_COUNT || s_hrpwm_ops[inst] == NULL) return -1;
    if (s_hrpwm_ops[inst]->config_reload_irq) return s_hrpwm_ops[inst]->config_reload_irq(callback);
    return -1;
}

int intf_hrpwm_enable_reload_irq(intf_hrpwm_inst_t inst)
{
    if (inst >= HRPWM_INSTANCE_COUNT || s_hrpwm_ops[inst] == NULL) return -1;
    if (s_hrpwm_ops[inst]->enable_reload_irq) return s_hrpwm_ops[inst]->enable_reload_irq();
    return -1;
}

int intf_hrpwm_disable_reload_irq(intf_hrpwm_inst_t inst)
{
    if (inst >= HRPWM_INSTANCE_COUNT || s_hrpwm_ops[inst] == NULL) return -1;
    if (s_hrpwm_ops[inst]->disable_reload_irq) return s_hrpwm_ops[inst]->disable_reload_irq();
    return -1;
}

ATTR_RAMFUNC
int intf_hrpwm_set_phase(const intf_hrpwm_phase_cfg_t *cfg)
{
    if (cfg == NULL || cfg->inst >= HRPWM_INSTANCE_COUNT || s_hrpwm_ops[cfg->inst] == NULL) return -1;
    if (s_hrpwm_ops[cfg->inst]->set_phase) return s_hrpwm_ops[cfg->inst]->set_phase(cfg);
    return -1;
}

int intf_hrpwm_config_phase_limit(intf_hrpwm_inst_t inst, const intf_hrpwm_phase_limit_t *limit)
{
    if (inst >= HRPWM_INSTANCE_COUNT || s_hrpwm_ops[inst] == NULL) return -1;
    if (s_hrpwm_ops[inst]->config_phase_limit) return s_hrpwm_ops[inst]->config_phase_limit(limit);
    return -1;
}

int intf_hrpwm_config_trigger_cmp(intf_hrpwm_inst_t inst, uint8_t cmp_index, uint32_t delay_ns)
{
    if (inst >= HRPWM_INSTANCE_COUNT || s_hrpwm_ops[inst] == NULL) return -1;
    if (s_hrpwm_ops[inst]->config_trigger_cmp) return s_hrpwm_ops[inst]->config_trigger_cmp(cmp_index, delay_ns);
    return -1;
}

ATTR_RAMFUNC
int intf_hrpwm_set_trigger_cmp_delay(intf_hrpwm_inst_t inst, uint8_t cmp_index, uint32_t delay_ns)
{
    if (inst >= HRPWM_INSTANCE_COUNT || s_hrpwm_ops[inst] == NULL) return -1;
    if (s_hrpwm_ops[inst]->set_trigger_cmp_delay) return s_hrpwm_ops[inst]->set_trigger_cmp_delay(cmp_index, delay_ns);
    return -1;
}

/* ============================================================================
 * GPTMR Interface
 * ============================================================================ */

#define GPTMR_INSTANCE_COUNT (4U)

static const intf_gptmr_t *s_gptmr_ops[GPTMR_INSTANCE_COUNT] = {NULL};

int intf_gptmr_register(const intf_gptmr_t *ops)
{
    if (ops == NULL || ops->instance_id >= GPTMR_INSTANCE_COUNT) return -1;
    s_gptmr_ops[ops->instance_id] = ops;
    return 0;
}

/**
 * @brief 按通道号查找所属实例的 GPTMR 实现
 * @param ch 通道号
 * @return 接口实现指针；NULL = 未注册/通道越界
 */
static const intf_gptmr_t *gptmr_get_ops(intf_gptmr_ch_t ch)
{
    uint8_t inst = ch / 4U;
    if (inst >= GPTMR_INSTANCE_COUNT || s_gptmr_ops[inst] == NULL) return NULL;
    return s_gptmr_ops[inst];
}

int intf_gptmr_init(intf_gptmr_ch_t ch, const intf_gptmr_cfg_t *cfg)
{
    const intf_gptmr_t *ops = gptmr_get_ops(ch);
    if (ops && ops->init) return ops->init(ch, cfg);
    return -1;
}

int intf_gptmr_start(intf_gptmr_ch_t ch)
{
    const intf_gptmr_t *ops = gptmr_get_ops(ch);
    if (ops && ops->start) return ops->start(ch);
    return -1;
}

int intf_gptmr_stop(intf_gptmr_ch_t ch)
{
    const intf_gptmr_t *ops = gptmr_get_ops(ch);
    if (ops && ops->stop) return ops->stop(ch);
    return -1;
}

int intf_gptmr_set_duty(intf_gptmr_ch_t ch, float duty)
{
    const intf_gptmr_t *ops = gptmr_get_ops(ch);
    if (ops && ops->set_duty) return ops->set_duty(ch, duty);
    return -1;
}

int intf_gptmr_set_frequency(intf_gptmr_ch_t ch, uint32_t frequency_hz)
{
    const intf_gptmr_t *ops = gptmr_get_ops(ch);
    if (ops && ops->set_frequency) return ops->set_frequency(ch, frequency_hz);
    return -1;
}

int intf_gptmr_force_low(intf_gptmr_ch_t ch)
{
    const intf_gptmr_t *ops = gptmr_get_ops(ch);
    if (ops && ops->force_low) return ops->force_low(ch);
    return -1;
}

int intf_gptmr_force_release(intf_gptmr_ch_t ch)
{
    const intf_gptmr_t *ops = gptmr_get_ops(ch);
    if (ops && ops->force_release) return ops->force_release(ch);
    return -1;
}

int intf_gptmr_capture_poll(intf_gptmr_ch_t ch, intf_gptmr_capture_t *capture)
{
    const intf_gptmr_t *ops = gptmr_get_ops(ch);
    if (ops && ops->capture_poll) return ops->capture_poll(ch, capture);
    return -1;
}

/* ============================================================================
 * TRGM Interface
 * ============================================================================ */

static const intf_trgm_t *s_trgm_ops = NULL;

int intf_trgm_register(const intf_trgm_t *ops)
{
    if (ops == NULL) return -1;
    s_trgm_ops = ops;
    return 0;
}

int intf_trgm_connect(intf_trgm_src_t src, intf_trgm_dst_t dst)
{
    if (s_trgm_ops && s_trgm_ops->connect) return s_trgm_ops->connect(src, dst);
    return -1;
}

/* ============================================================================
 * ADC Interface
 * ============================================================================ */

static const intf_adc_t *s_adc_ops[INTF_ADC_INSTANCE_COUNT] = {NULL};

int intf_adc_register(const intf_adc_t *ops)
{
    if (ops == NULL || ops->instance_id >= INTF_ADC_INSTANCE_COUNT) return -1;
    s_adc_ops[ops->instance_id] = ops;
    return 0;
}

/**
 * @brief 按通道号查找所属实例的 ADC 实现
 * @param ch 通道号
 * @return 接口实现指针；NULL = 未注册/通道越界
 */
static const intf_adc_t *adc_get_ops_by_ch(intf_adc_ch_t ch)
{
    uint8_t inst = INTF_ADC_CH_INST(ch);
    if (inst >= INTF_ADC_INSTANCE_COUNT) return NULL;
    return s_adc_ops[inst];
}

int intf_adc_init(intf_adc_ch_t ch, const intf_adc_cfg_t *cfg)
{
    const intf_adc_t *ops = adc_get_ops_by_ch(ch);
    if (ops && ops->init) return ops->init(ch, cfg);
    return -1;
}

int intf_adc_read(intf_adc_ch_t ch, uint16_t *value)
{
    const intf_adc_t *ops = adc_get_ops_by_ch(ch);
    if (ops && ops->read) return ops->read(ch, value);
    return -1;
}

int intf_adc_read_voltage(intf_adc_ch_t ch, float *voltage_mv)
{
    const intf_adc_t *ops = adc_get_ops_by_ch(ch);
    if (ops && ops->read_voltage) return ops->read_voltage(ch, voltage_mv);
    return -1;
}

int intf_adc_start(intf_adc_ch_t ch)
{
    const intf_adc_t *ops = adc_get_ops_by_ch(ch);
    if (ops && ops->start) return ops->start(ch);
    return -1;
}

int intf_adc_stop(intf_adc_ch_t ch)
{
    const intf_adc_t *ops = adc_get_ops_by_ch(ch);
    if (ops && ops->stop) return ops->stop(ch);
    return -1;
}

extern void adc_wdog_reenable(uint8_t inst, uint8_t ch);

void intf_adc_wdog_reenable(intf_adc_ch_t ch)
{
    uint8_t inst   = INTF_ADC_CH_INST(ch);
    uint8_t ch_idx = INTF_ADC_CH_IDX(ch);
    if (inst < INTF_ADC_INSTANCE_COUNT) {
        adc_wdog_reenable(inst, ch_idx);
    }
}

/**
 * @brief 获取 ADC 诊断快照（弱默认实现）
 * @param snapshot 输出诊断快照
 * @return 0 = 成功；-1 = 未实现
 */
__attribute__((weak)) int adc_get_diag_snapshot(intf_adc_diag_snapshot_t *snapshot)
{
    (void)snapshot;
    return -1;
}

int intf_adc_get_diag_snapshot(intf_adc_diag_snapshot_t *snapshot)
{
    return adc_get_diag_snapshot(snapshot);
}

/**
 * @brief 清零 ADC 诊断最大值（弱默认实现，空操作）
 */
__attribute__((weak)) void adc_reset_diag_max(void) { }

void intf_adc_reset_diag_max(void)
{
    adc_reset_diag_max();
}

/* ============================================================================
 * UART Interface（设备对象注册表，风格 A）
 * ============================================================================ */

#define UART_INSTANCE_COUNT (4U)

static const intf_uart_t *s_uart_devs[UART_INSTANCE_COUNT] = { NULL };

int intf_uart_register(const intf_uart_t *dev)
{
    if ((dev == NULL) || (dev->instance_id >= UART_INSTANCE_COUNT)) return -1;
    s_uart_devs[dev->instance_id] = dev;
    return 0;
}

const intf_uart_t *intf_uart_get(intf_uart_port_t port)
{
    if (port >= UART_INSTANCE_COUNT) return NULL;
    return s_uart_devs[port];
}

/* ============================================================================
 * GPIO Interface
 * ============================================================================ */

static const intf_gpio_t *s_gpio_ops = NULL;

int intf_gpio_register(const intf_gpio_t *ops)
{
    if (ops == NULL) return -1;
    s_gpio_ops = ops;
    return 0;
}

int intf_gpio_init(const intf_gpio_cfg_t *cfg)
{
    if (s_gpio_ops && s_gpio_ops->init) return s_gpio_ops->init(cfg);
    return -1;
}

int intf_gpio_set_level(intf_gpio_pin_t pin, intf_gpio_level_t level)
{
    if (s_gpio_ops && s_gpio_ops->set_level) return s_gpio_ops->set_level(pin, level);
    return -1;
}

int intf_gpio_get_level(intf_gpio_pin_t pin, intf_gpio_level_t *level)
{
    if (s_gpio_ops && s_gpio_ops->get_level) return s_gpio_ops->get_level(pin, level);
    return -1;
}

int intf_gpio_toggle(intf_gpio_pin_t pin)
{
    if (s_gpio_ops && s_gpio_ops->toggle) return s_gpio_ops->toggle(pin);
    return -1;
}

/* ============================================================================
 * CAN Interface
 * ============================================================================ */

#define CAN_INSTANCE_COUNT (4U)

static const intf_can_t *s_can_ops[CAN_INSTANCE_COUNT] = {NULL};

int intf_can_register(const intf_can_t *ops)
{
    if (ops == NULL || ops->instance_id >= CAN_INSTANCE_COUNT) return -1;
    s_can_ops[ops->instance_id] = ops;
    return 0;
}

int intf_can_init(intf_can_inst_t inst, const intf_can_cfg_t *cfg)
{
    if (inst >= CAN_INSTANCE_COUNT || s_can_ops[inst] == NULL) return -1;
    if (s_can_ops[inst]->init) return s_can_ops[inst]->init(cfg);
    return -1;
}

void intf_can_deinit(intf_can_inst_t inst)
{
    if (inst < CAN_INSTANCE_COUNT && s_can_ops[inst] && s_can_ops[inst]->deinit)
        s_can_ops[inst]->deinit();
}

int intf_can_send(intf_can_inst_t inst, const intf_can_frame_t *frame,
                   uint32_t timeout_ms)
{
    if (inst >= CAN_INSTANCE_COUNT || s_can_ops[inst] == NULL) return -1;
    if (s_can_ops[inst]->send) return s_can_ops[inst]->send(frame, timeout_ms);
    return -1;
}

int intf_can_send_nonblocking(intf_can_inst_t inst,
                               const intf_can_frame_t *frame,
                               uint8_t *fifo_idx)
{
    if (inst >= CAN_INSTANCE_COUNT || s_can_ops[inst] == NULL) return -1;
    if (s_can_ops[inst]->send_nonblocking) return s_can_ops[inst]->send_nonblocking(frame, fifo_idx);
    return -1;
}

int intf_can_send_add_request(intf_can_inst_t inst, uint8_t fifo_idx)
{
    if (inst >= CAN_INSTANCE_COUNT || s_can_ops[inst] == NULL) return -1;
    if (s_can_ops[inst]->send_add_request) return s_can_ops[inst]->send_add_request(fifo_idx);
    return -1;
}

int intf_can_receive(intf_can_inst_t inst, intf_can_frame_t *frame,
                      uint32_t timeout_ms)
{
    if (inst >= CAN_INSTANCE_COUNT || s_can_ops[inst] == NULL) return -1;
    if (s_can_ops[inst]->receive) return s_can_ops[inst]->receive(frame, timeout_ms);
    return -1;
}

int intf_can_receive_nonblocking(intf_can_inst_t inst,
                                  intf_can_frame_t *frame)
{
    if (inst >= CAN_INSTANCE_COUNT || s_can_ops[inst] == NULL) return -1;
    if (s_can_ops[inst]->receive_nonblocking) return s_can_ops[inst]->receive_nonblocking(frame);
    return -1;
}

int intf_can_config_filter(intf_can_inst_t inst, uint32_t index,
                            const intf_can_filter_elem_t *elem)
{
    if (inst >= CAN_INSTANCE_COUNT || s_can_ops[inst] == NULL) return -1;
    if (s_can_ops[inst]->config_filter) return s_can_ops[inst]->config_filter(index, elem);
    return -1;
}

int intf_can_enable_interrupt(intf_can_inst_t inst, uint32_t event_mask)
{
    if (inst >= CAN_INSTANCE_COUNT || s_can_ops[inst] == NULL) return -1;
    if (s_can_ops[inst]->enable_interrupt) return s_can_ops[inst]->enable_interrupt(event_mask);
    return -1;
}

int intf_can_disable_interrupt(intf_can_inst_t inst, uint32_t event_mask)
{
    if (inst >= CAN_INSTANCE_COUNT || s_can_ops[inst] == NULL) return -1;
    if (s_can_ops[inst]->disable_interrupt) return s_can_ops[inst]->disable_interrupt(event_mask);
    return -1;
}

int intf_can_config_irq_callback(intf_can_inst_t inst,
                                  intf_can_irq_callback_t cb,
                                  void *user_data)
{
    if (inst >= CAN_INSTANCE_COUNT || s_can_ops[inst] == NULL) return -1;
    if (s_can_ops[inst]->config_irq_callback) return s_can_ops[inst]->config_irq_callback(cb, user_data);
    return -1;
}

int intf_can_get_status(intf_can_inst_t inst, intf_can_status_t *status)
{
    if (inst >= CAN_INSTANCE_COUNT || s_can_ops[inst] == NULL) return -1;
    if (s_can_ops[inst]->get_status) return s_can_ops[inst]->get_status(status);
    return -1;
}

int intf_can_read_tx_event(intf_can_inst_t inst, intf_can_tx_event_t *tx_evt)
{
    if (inst >= CAN_INSTANCE_COUNT || s_can_ops[inst] == NULL) return -1;
    if (s_can_ops[inst]->read_tx_event) return s_can_ops[inst]->read_tx_event(tx_evt);
    return -1;
}

int intf_can_get_timestamp(intf_can_inst_t inst,
                            const intf_can_tx_event_t *tx_evt,
                            intf_can_timestamp_t *ts)
{
    if (inst >= CAN_INSTANCE_COUNT || s_can_ops[inst] == NULL) return -1;
    if (s_can_ops[inst]->get_timestamp) return s_can_ops[inst]->get_timestamp(tx_evt, ts);
    return -1;
}

/* ============================================================================
 * SYNT Interface
 * ============================================================================ */

static const intf_synt_t *s_synt_ops = NULL;

int intf_synt_register(const intf_synt_t *ops)
{
    if (ops == NULL) return -1;
    s_synt_ops = ops;
    return 0;
}

int intf_synt_init(const intf_synt_cfg_t *cfg)
{
    if (s_synt_ops && s_synt_ops->init) return s_synt_ops->init(cfg);
    return -1;
}

int intf_synt_start(void)
{
    if (s_synt_ops && s_synt_ops->start) return s_synt_ops->start();
    return -1;
}

int intf_synt_stop(void)
{
    if (s_synt_ops && s_synt_ops->stop) return s_synt_ops->stop();
    return -1;
}

int intf_synt_reset(void)
{
    if (s_synt_ops && s_synt_ops->reset) return s_synt_ops->reset();
    return -1;
}

int intf_synt_set_reload(uint32_t reload_count)
{
    if (s_synt_ops && s_synt_ops->set_reload) return s_synt_ops->set_reload(reload_count);
    return -1;
}

int intf_synt_set_compare(intf_synt_ch_t ch, uint32_t cmp_count)
{
    if (s_synt_ops && s_synt_ops->set_compare) return s_synt_ops->set_compare(ch, cmp_count);
    return -1;
}

uint32_t intf_synt_get_count(void)
{
    if (s_synt_ops && s_synt_ops->get_count) return s_synt_ops->get_count();
    return 0;
}

/* ============================================================================
 * USB CDC Interface（单实例设备对象，风格 A）
 * ============================================================================ */

static const intf_usb_cdc_t *s_usb_cdc_dev = NULL;

int intf_usb_cdc_register(const intf_usb_cdc_t *dev)
{
    if (dev == NULL) return -1;
    s_usb_cdc_dev = dev;
    return 0;
}

const intf_usb_cdc_t *intf_usb_cdc_get(void)
{
    return s_usb_cdc_dev;
}

/* ============================================================================
 * SPI Interface（设备对象注册表，风格 A）
 * ============================================================================ */

#define SPI_INSTANCE_COUNT (4U)

static const intf_spi_t *s_spi_devs[SPI_INSTANCE_COUNT] = { NULL };

int intf_spi_register(const intf_spi_t *dev)
{
    if ((dev == NULL) || (dev->instance_id >= SPI_INSTANCE_COUNT)) return -1;
    s_spi_devs[dev->instance_id] = dev;
    return 0;
}

const intf_spi_t *intf_spi_get(intf_spi_bus_t bus)
{
    if (bus >= SPI_INSTANCE_COUNT) return NULL;
    return s_spi_devs[bus];
}

/* ============================================================================
 * Encoder Interface（设备对象注册表，风格 A）
 * ============================================================================ */

#define ENCODER_INSTANCE_COUNT (4U)

static const intf_encoder_t *s_encoder_devs[ENCODER_INSTANCE_COUNT] = { NULL };

int intf_encoder_register(const intf_encoder_t *dev)
{
    if ((dev == NULL) || (dev->instance_id >= ENCODER_INSTANCE_COUNT)) return -1;
    s_encoder_devs[dev->instance_id] = dev;
    return 0;
}

const intf_encoder_t *intf_encoder_get(intf_encoder_id_t id)
{
    if (id >= ENCODER_INSTANCE_COUNT) return NULL;
    return s_encoder_devs[id];
}

/* ============================================================================
 * Flash Interface（单实例设备对象，风格 A）
 * ============================================================================ */

static const intf_flash_t *s_flash_dev = NULL;

int intf_flash_register(const intf_flash_t *dev)
{
    if (dev == NULL) return -1;
    s_flash_dev = dev;
    return 0;
}

const intf_flash_t *intf_flash_get(void)
{
    return s_flash_dev;
}
