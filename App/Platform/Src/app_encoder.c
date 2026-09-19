/*
 * App Encoder - 编码器平台封装（双 KTH7823）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 板级映射：
 *   APP_ENCODER_ROTOR  -> SPI3（PA10-13），转子 1:1
 *   APP_ENCODER_OUTPUT -> SPI1（PA26-29），出轴 49:50（游标）
 *
 * 实时性：read_raw 为阻塞短操作（实测 ~7µs），无打印/动态分配；
 *         设备对象在 init 时解析并缓存，热路径无注册表查表；
 *         每实例单所有者，不可在多上下文并发调用。
 */

#include "app_encoder.h"

#include "intf_encoder.h"
#include "intf_spi.h"

#define APP_ENCODER_SCLK_HZ (10000000U) /* KTH7823 上限（TSCK≥100ns） */

/* 板级映射：转子 -> SPI3，出轴 -> SPI1（SoC 实例号） */
static const uint8_t s_encoder_bus[APP_ENCODER_COUNT] = { 3U, 1U };

/* init 时解析的设备对象（热路径直接调用，不再查表） */
static const intf_encoder_t *s_enc_dev[APP_ENCODER_COUNT];
static const intf_spi_t *s_spi_dev[APP_ENCODER_COUNT];

static float s_rad_scale[APP_ENCODER_COUNT];
static float s_deg_scale[APP_ENCODER_COUNT];

/* 驱动注册（App 层不含 hpm_* 头文件，沿用既有 extern 约定） */
extern void hpm_spi_driver_register(void);
extern void hpm_kth7823_driver_register(void);

int app_encoder_init(void)
{
    int ret = 0;

    hpm_spi_driver_register();
    hpm_kth7823_driver_register();

    for (uint8_t i = 0U; i < (uint8_t) APP_ENCODER_COUNT; i++) {
        intf_encoder_cfg_t cfg = {
            .bus = s_encoder_bus[i],
            .sclk_hz = APP_ENCODER_SCLK_HZ,
        };
        intf_encoder_info_t info;

        /* 默认 16bit；驱动返回实际分辨率后重算换算系数 */
        s_rad_scale[i] = 6.283185307179586f / 65536.0f;
        s_deg_scale[i] = 360.0f / 65536.0f;

        s_enc_dev[i] = intf_encoder_get((intf_encoder_id_t) i);
        s_spi_dev[i] = intf_spi_get(s_encoder_bus[i]);
        if ((s_enc_dev[i] == NULL) || (s_spi_dev[i] == NULL)) {
            ret = -1;
            continue;
        }

        if (s_enc_dev[i]->init(&cfg) != 0) {
            ret = -1;
            continue;
        }
        if (s_enc_dev[i]->get_info(&info) == 0) {
            if ((info.resolution_bits > 0U) && (info.resolution_bits <= 31U)) {
                float counts = (float)(1UL << info.resolution_bits);

                s_rad_scale[i] = 6.283185307179586f / counts;
                s_deg_scale[i] = 360.0f / counts;
            }
        }
    }

    return ret;
}

int app_encoder_read_raw(app_encoder_id_t id, uint16_t *raw)
{
    if ((id >= APP_ENCODER_COUNT) || (raw == NULL) || (s_enc_dev[id] == NULL)) {
        return -1;
    }
    return s_enc_dev[id]->read_raw(raw);
}

int app_encoder_read_rad(app_encoder_id_t id, float *rad)
{
    uint16_t raw;

    if ((id >= APP_ENCODER_COUNT) || (rad == NULL)) {
        return -1;
    }
    if (app_encoder_read_raw(id, &raw) != 0) {
        return -1;
    }

    *rad = (float) raw * s_rad_scale[id];
    return 0;
}

int app_encoder_read_deg(app_encoder_id_t id, float *deg)
{
    uint16_t raw;

    if ((id >= APP_ENCODER_COUNT) || (deg == NULL)) {
        return -1;
    }
    if (app_encoder_read_raw(id, &raw) != 0) {
        return -1;
    }

    *deg = (float) raw * s_deg_scale[id];
    return 0;
}

int app_encoder_read_reg(app_encoder_id_t id, uint8_t addr, uint8_t *val)
{
    if ((id >= APP_ENCODER_COUNT) || (val == NULL) || (s_enc_dev[id] == NULL)) {
        return -1;
    }
    return s_enc_dev[id]->read_reg(addr, val);
}

int app_encoder_set_zero(app_encoder_id_t id, uint16_t zero)
{
    if ((id >= APP_ENCODER_COUNT) || (s_enc_dev[id] == NULL)) {
        return -1;
    }
    return s_enc_dev[id]->set_zero(zero);
}

int app_encoder_set_direction(app_encoder_id_t id, bool cw_increasing)
{
    if ((id >= APP_ENCODER_COUNT) || (s_enc_dev[id] == NULL)) {
        return -1;
    }
    return s_enc_dev[id]->set_direction(cw_increasing);
}

uint32_t app_encoder_get_error_count(app_encoder_id_t id)
{
    if ((id >= APP_ENCODER_COUNT) || (s_enc_dev[id] == NULL)) {
        return 0U;
    }
    return s_enc_dev[id]->get_error_count();
}

uint32_t app_encoder_get_sclk_hz(app_encoder_id_t id)
{
    if ((id >= APP_ENCODER_COUNT) || (s_spi_dev[id] == NULL)) {
        return 0U;
    }
    return s_spi_dev[id]->get_sclk_hz();
}
