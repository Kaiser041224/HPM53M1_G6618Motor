/**
 * @file    mock_deps.h
 * @brief   USB 协议主机自测：外部依赖替身状态
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef MOCK_DEPS_H
#define MOCK_DEPS_H

#include "app_adc.h"
#include "app_analog_signal.h"
#include "app_boot.h"
#include "app_encoder.h"
#include "app_foc.h"
#include "app_foc_current.h"
#include "app_motion.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 被测模块可见的全部外部状态（测试直接读写）
 */
typedef struct {
    /* bulk 通道 */
    uint8_t rx[2048];   /**< 待喂入的请求字节 */
    uint16_t rx_len;    /**< 已喂入长度 */
    uint16_t rx_pos;    /**< 已消费长度 */
    uint8_t tx[2048];   /**< 最近一次成功写出的帧 */
    uint16_t tx_len;    /**< 最近帧长度 */
    int tx_count;       /**< 成功写出的帧数 */
    bool ready;         /**< app_usb_bulk_ready() 返回值 */
    int write_rc;       /**< app_usb_bulk_write() 返回值（0/-1） */

    /* 控制/保护 */
    app_foc_state_t foc_state;
    int foc_enable_rc;
    int stop_rc;
    bool stop_pending;

    /* motion */
    int motion_rc;
    int motion_call;    /**< 0=无, 1=torque, 2=speed, 3=position */
    float motion_arg;   /**< 最近一次设定值（已换算为设备单位） */
    int release_count;  /**< app_motion_release_output 调用次数 */

    /* 快照 */
    app_foc_current_snapshot_t foc_snap;
    app_motion_snapshot_t motion_snap;
    app_analog_values_t analog;
    bool analog_ok;
    float ntc0_c;
    float ntc1_c;
    bool ntc_valid;

    app_encoder_rotor_snapshot_t enc_pair;
    uint16_t enc_output_raw;
    float rotor_deg;
    float output_deg;

    /* 故障 */
    uint32_t fault_codes;
    uint32_t fault_latched;
    int fault_clear_rc;

    /* OTA / DFU */
    bool boot_status_ok;      /**< app_boot_get_status() 返回 0 还是 -1 */
    app_boot_status_t boot_status;

    /* 时钟 */
    uint32_t cycles;
} mock_state_t;

extern mock_state_t g_mock;

/** @brief 复位全部替身状态到可用的默认值 */
void mock_reset(void);

#endif /* MOCK_DEPS_H */
