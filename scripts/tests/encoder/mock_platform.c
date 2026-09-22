/**
 * @file    mock_platform.c
 * @brief   宿主测试替身：编码器/SPI 接口摘要 + 参数存储/时钟桩
 * @author  Kaiser
 *
 * 仅用于宿主机自测，不参与目标固件。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_encoder.h"
#include "intf_spi.h"
#include "app_gptmr.h"

#include <stddef.h>
#include <string.h>

/* 可观测计数（测试读取） */
int g_mock_enc_read_count[2];
int g_mock_enc_read_reg_count[2];
int g_mock_enc_read_fail[2];
uint16_t g_mock_enc_raw[2];
uint32_t g_mock_cycle;

void mock_reset(void) {
    memset(g_mock_enc_read_count, 0, sizeof(g_mock_enc_read_count));
    memset(g_mock_enc_read_reg_count, 0, sizeof(g_mock_enc_read_reg_count));
    memset(g_mock_enc_read_fail, 0, sizeof(g_mock_enc_read_fail));
    memset(g_mock_enc_raw, 0, sizeof(g_mock_enc_raw));
    g_mock_cycle = 0U;
}

/* ---- 编码器替身：两个实例各自函数 ---- */
static int enc0_init(const intf_encoder_cfg_t *cfg) { (void)cfg; return 0; }
static void enc0_deinit(void) {}
static int enc0_read_raw(uint16_t *raw) {
    g_mock_enc_read_count[0]++;
    if (g_mock_enc_read_fail[0]) {
        return -1;
    }
    *raw = g_mock_enc_raw[0];
    return 0;
}
static int enc0_read_reg(uint8_t a, uint8_t *v) { g_mock_enc_read_reg_count[0]++; (void)a; *v = 0x80U; return 0; }
static int enc0_write_reg(uint8_t a, uint8_t v) { (void)a; (void)v; return 0; }
static int enc0_set_zero(uint16_t z) { (void)z; return 0; }
static int enc0_set_direction(bool cw) { (void)cw; return 0; }
static int enc0_get_info(intf_encoder_info_t *info) { info->resolution_bits = 16U; info->has_registers = true; return 0; }
static uint32_t enc0_get_error_count(void) { return 0U; }

static int enc1_init(const intf_encoder_cfg_t *cfg) { (void)cfg; return 0; }
static void enc1_deinit(void) {}
static int enc1_read_raw(uint16_t *raw) {
    g_mock_enc_read_count[1]++;
    if (g_mock_enc_read_fail[1]) {
        return -1;
    }
    *raw = g_mock_enc_raw[1];
    return 0;
}
static int enc1_read_reg(uint8_t a, uint8_t *v) { g_mock_enc_read_reg_count[1]++; (void)a; *v = 0x80U; return 0; }
static int enc1_write_reg(uint8_t a, uint8_t v) { (void)a; (void)v; return 0; }
static int enc1_set_zero(uint16_t z) { (void)z; return 0; }
static int enc1_set_direction(bool cw) { (void)cw; return 0; }
static int enc1_get_info(intf_encoder_info_t *info) { info->resolution_bits = 16U; info->has_registers = true; return 0; }
static uint32_t enc1_get_error_count(void) { return 0U; }

static const intf_encoder_t s_enc0 = {
    .instance_id = 0U,
    .init = enc0_init, .deinit = enc0_deinit, .read_raw = enc0_read_raw,
    .read_reg = enc0_read_reg, .write_reg = enc0_write_reg, .set_zero = enc0_set_zero,
    .set_direction = enc0_set_direction, .get_info = enc0_get_info,
    .get_error_count = enc0_get_error_count,
};
static const intf_encoder_t s_enc1 = {
    .instance_id = 1U,
    .init = enc1_init, .deinit = enc1_deinit, .read_raw = enc1_read_raw,
    .read_reg = enc1_read_reg, .write_reg = enc1_write_reg, .set_zero = enc1_set_zero,
    .set_direction = enc1_set_direction, .get_info = enc1_get_info,
    .get_error_count = enc1_get_error_count,
};

const intf_encoder_t *intf_encoder_get(intf_encoder_id_t id) {
    if (id == 0U) return &s_enc0;
    if (id == 1U) return &s_enc1;
    return NULL;
}
int intf_encoder_register(const intf_encoder_t *dev) { (void)dev; return 0; }

/* ---- SPI 替身（app_encoder 只取 sclk） ---- */
static uint32_t spi_get_sclk(void) { return 5000000U; }
int intf_spi_get_impl_dummy;
static const intf_spi_t s_spi = {
    .instance_id = 0U,
    .init = NULL, .transfer = NULL, .deinit = NULL, .get_sclk_hz = spi_get_sclk,
};
const intf_spi_t *intf_spi_get(uint8_t bus) { (void)bus; return &s_spi; }
int intf_spi_register(const intf_spi_t *dev) { (void)dev; return 0; }

/* ---- 驱动注册桩 ---- */
void hpm_spi_driver_register(void) {}
void hpm_kth7823_driver_register(void) {}

/* ---- 参数存储桩（无 flash：load 总是未命中） ---- */
int app_param_init(void) { return 0; }
bool app_param_is_ready(void) { return false; }
int app_param_load(uint32_t key, void *buf, size_t len) {
    (void)key; (void)buf; (void)len;
    return -1;
}
int app_param_store(uint32_t key, const void *buf, size_t len) {
    (void)key; (void)buf; (void)len;
    return -1;
}

/* ---- 时钟桩 ---- */
uint32_t intf_clock_get_cycle(void) { return g_mock_cycle; }

/* ---- GPTMR 桩（app_encoder_sampler_start 引用） ---- */
int app_gptmr_register_callback(app_gptmr_ch_t ch, app_gptmr_callback_t cb) {
    (void)ch; (void)cb;
    return 0;
}
int app_gptmr_start(app_gptmr_ch_t ch) { (void)ch; return 0; }
