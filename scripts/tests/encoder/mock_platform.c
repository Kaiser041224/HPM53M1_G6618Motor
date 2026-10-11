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
#include "intf_trgm.h"

#include <stddef.h>
#include <string.h>

/* 可观测计数（测试读取） */
int g_mock_enc_read_count[2];
int g_mock_enc_read_reg_count[2];
int g_mock_enc_read_fail[2];
uint16_t g_mock_enc_raw[2];
uint32_t g_mock_cycle;
int g_mock_enc_sync_arm_count[2];
int g_mock_enc_sync_fetch_count[2];
int g_mock_enc_sync_fetch_fail[2];
uint8_t g_mock_enc_sync_trig[2];
int g_mock_trgm_connect_count;
intf_trgm_src_t g_mock_trgm_src;
intf_trgm_dst_t g_mock_trgm_dst;

void mock_reset(void) {
    memset(g_mock_enc_read_count, 0, sizeof(g_mock_enc_read_count));
    memset(g_mock_enc_read_reg_count, 0, sizeof(g_mock_enc_read_reg_count));
    memset(g_mock_enc_read_fail, 0, sizeof(g_mock_enc_read_fail));
    memset(g_mock_enc_raw, 0, sizeof(g_mock_enc_raw));
    memset(g_mock_enc_sync_arm_count, 0, sizeof(g_mock_enc_sync_arm_count));
    memset(g_mock_enc_sync_fetch_count, 0, sizeof(g_mock_enc_sync_fetch_count));
    memset(g_mock_enc_sync_fetch_fail, 0, sizeof(g_mock_enc_sync_fetch_fail));
    memset(g_mock_enc_sync_trig, 0, sizeof(g_mock_enc_sync_trig));
    g_mock_trgm_connect_count = 0;
    g_mock_trgm_src = (intf_trgm_src_t)0;
    g_mock_trgm_dst = (intf_trgm_dst_t)0;
    g_mock_cycle = 0U;
}

/* ---- 同步采样替身（硬件触发帧：arm 一次，每拍 fetch 一次） ---- */
static int mock_sync_arm(uint8_t id, uint8_t trig) {
    g_mock_enc_sync_arm_count[id]++;
    g_mock_enc_sync_trig[id] = trig;
    return 0;
}
static int mock_sync_fetch(uint8_t id, uint16_t *raw, uint32_t wait_cycles) {
    (void)wait_cycles;
    g_mock_enc_sync_fetch_count[id]++;
    if (g_mock_enc_sync_fetch_fail[id]) {
        return -1;
    }
    *raw = g_mock_enc_raw[id];
    return 0;
}
static int enc0_sync_arm(uint8_t trig) { return mock_sync_arm(0U, trig); }
static int enc0_sync_fetch(uint16_t *raw, uint32_t w) { return mock_sync_fetch(0U, raw, w); }
static void enc0_sync_disarm(void) {}
static int enc1_sync_arm(uint8_t trig) { return mock_sync_arm(1U, trig); }
static int enc1_sync_fetch(uint16_t *raw, uint32_t w) { return mock_sync_fetch(1U, raw, w); }
static void enc1_sync_disarm(void) {}

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
    .sync_arm = enc0_sync_arm, .sync_fetch = enc0_sync_fetch, .sync_disarm = enc0_sync_disarm,
};
static const intf_encoder_t s_enc1 = {
    .instance_id = 1U,
    .init = enc1_init, .deinit = enc1_deinit, .read_raw = enc1_read_raw,
    .read_reg = enc1_read_reg, .write_reg = enc1_write_reg, .set_zero = enc1_set_zero,
    .set_direction = enc1_set_direction, .get_info = enc1_get_info,
    .get_error_count = enc1_get_error_count,
    .sync_arm = enc1_sync_arm, .sync_fetch = enc1_sync_fetch, .sync_disarm = enc1_sync_disarm,
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

uint32_t intf_clock_get_cpu_freq(void) { return 480000000U; }

/* ---- TRGM 桩（app_encoder_sync_start 路由硬件触发） ---- */
int intf_trgm_connect(intf_trgm_src_t src, intf_trgm_dst_t dst) {
    g_mock_trgm_connect_count++;
    g_mock_trgm_src = src;
    g_mock_trgm_dst = dst;
    return 0;
}
