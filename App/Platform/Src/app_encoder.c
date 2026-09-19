/*
 * App Encoder - 编码器平台封装（双 KTH7823）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_encoder.h"

#include "intf_encoder.h"
#include "intf_spi.h"

#define APP_ENCODER_SCLK_HZ (10000000U) /* KTH7823 上限（TSCK≥100ns） */

/* 板级映射：转子 -> SPI3，出轴 -> SPI1（SoC 实例号） */
static const uint8_t s_encoder_bus[APP_ENCODER_COUNT] = { 3U, 1U };

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

        if (intf_encoder_init((intf_encoder_id_t) i, &cfg) != 0) {
            ret = -1;
            continue;
        }
        if (intf_encoder_get_info((intf_encoder_id_t) i, &info) == 0) {
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
    if ((id >= APP_ENCODER_COUNT) || (raw == NULL)) {
        return -1;
    }
    return intf_encoder_read_raw((intf_encoder_id_t) id, raw);
}

int app_encoder_read_rad(app_encoder_id_t id, float *rad)
{
    uint16_t raw;

    if ((id >= APP_ENCODER_COUNT) || (rad == NULL)) {
        return -1;
    }
    if (intf_encoder_read_raw((intf_encoder_id_t) id, &raw) != 0) {
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
    if (intf_encoder_read_raw((intf_encoder_id_t) id, &raw) != 0) {
        return -1;
    }

    *deg = (float) raw * s_deg_scale[id];
    return 0;
}

int app_encoder_read_reg(app_encoder_id_t id, uint8_t addr, uint8_t *val)
{
    if ((id >= APP_ENCODER_COUNT) || (val == NULL)) {
        return -1;
    }
    return intf_encoder_read_reg((intf_encoder_id_t) id, addr, val);
}

int app_encoder_set_zero(app_encoder_id_t id, uint16_t zero)
{
    if (id >= APP_ENCODER_COUNT) {
        return -1;
    }
    return intf_encoder_set_zero((intf_encoder_id_t) id, zero);
}

int app_encoder_set_direction(app_encoder_id_t id, bool cw_increasing)
{
    if (id >= APP_ENCODER_COUNT) {
        return -1;
    }
    return intf_encoder_set_direction((intf_encoder_id_t) id, cw_increasing);
}

uint32_t app_encoder_get_error_count(app_encoder_id_t id)
{
    if (id >= APP_ENCODER_COUNT) {
        return 0U;
    }
    return intf_encoder_get_error_count((intf_encoder_id_t) id);
}

uint32_t app_encoder_get_sclk_hz(app_encoder_id_t id)
{
    if (id >= APP_ENCODER_COUNT) {
        return 0U;
    }
    return intf_spi_get_sclk_hz(s_encoder_bus[id]);
}
