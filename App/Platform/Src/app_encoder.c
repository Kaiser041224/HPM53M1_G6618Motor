/**
 * @file    app_encoder.c
 * @brief   编码器平台封装（双 KTH7823）
 * @author  Kaiser
 *
 * 板级映射：
 *   APP_ENCODER_ROTOR  -> SPI3（PA10-13），转子 1:1
 *   APP_ENCODER_OUTPUT -> SPI1（PA26-29），出轴 49:50（游标）
 *
 * 零点：软件方案（app_param 键值存储，存 flash 参数区，掉电保持），
 *       不消耗编码器 MTP（Z 寄存器寿命仅 1000 次写）。
 *
 * 实时性：read_raw 为阻塞短操作（实测 ~7µs），无打印/动态分配；
 *         设备对象在 init 时解析并缓存，热路径无注册表查表；
 *         每实例单所有者，不可在多上下文并发调用。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_encoder.h"

#include "app_param.h"
#include "intf_encoder.h"
#include "intf_spi.h"

#include <string.h>

#define APP_ENCODER_SCLK_HZ (5000000U) /* 降为 5MHz：10MHz 台架实测坏帧率高（EMI） */

/** 机械角单步跳变上限 [deg]（25kHz 采样；5° ≈ 20000 rpm，远超实际转速） */
#define APP_ENCODER_JUMP_LIMIT_DEG (5.0f)

/**
 * @brief 编码器持久化参数（app_param 键 APP_PARAM_KEY_ENCODER 的数据布局）
 */
typedef struct {
    uint16_t zero[APP_ENCODER_COUNT]; /**< 软件零点（原始值） */
    uint16_t flags;                   /**< 预留（方向等） */
} encoder_param_t;

/* 板级映射：转子 -> SPI3，出轴 -> SPI1（SoC 实例号） */
static bool s_rotor_valid;
static uint32_t s_rotor_seq;      /* 成功采样序号（陈旧检测） */
static uint16_t s_rotor_prev_raw; /**< 上一有效原始值（跳变检测） */
static bool s_rotor_prev_valid;   /**< 上一有效值已建立 */
static int32_t s_rotor_jump_limit;/**< 单步跳变上限 [count]（init 时按分辨率算） */
static uint32_t s_rotor_jump_count; /**< 跳变（坏帧）计数 */
static const uint8_t s_encoder_bus[APP_ENCODER_COUNT] = {3U, 1U};

/* init 时解析的设备对象（热路径直接调用，不再查表） */
static const intf_encoder_t* s_encoder_dev[APP_ENCODER_COUNT];
static const intf_spi_t* s_spi_dev[APP_ENCODER_COUNT];

static float s_rad_scale[APP_ENCODER_COUNT];
static float s_deg_scale[APP_ENCODER_COUNT];
static uint16_t s_zero[APP_ENCODER_COUNT]; /* 软件零点（原始值） */
static bool s_param_loaded;                /* 是否从 flash 加载到有效记录 */

/* 驱动注册（App 层不含 hpm_* 头文件，沿用既有 extern 约定） */
extern void hpm_spi_driver_register(void);
extern void hpm_kth7823_driver_register(void);

int app_encoder_init(void) {
    int ret = 0;

    hpm_spi_driver_register();
    hpm_kth7823_driver_register();

    for (uint8_t i = 0U; i < (uint8_t)APP_ENCODER_COUNT; i++) {
        intf_encoder_cfg_t cfg = {
            .bus = s_encoder_bus[i],
            .sclk_hz = APP_ENCODER_SCLK_HZ,
        };
        intf_encoder_info_t info;

        /* 默认 16bit；驱动返回实际分辨率后重算换算系数 */
        s_rad_scale[i] = 6.283185307179586f / 65536.0f;
        if (i == (uint8_t)APP_ENCODER_ROTOR) {
            s_rotor_jump_limit = (int32_t)(65536.0f * (APP_ENCODER_JUMP_LIMIT_DEG / 360.0f));
        }
        s_deg_scale[i] = 360.0f / 65536.0f;

        s_encoder_dev[i] = intf_encoder_get((intf_encoder_id_t)i);
        s_spi_dev[i] = intf_spi_get(s_encoder_bus[i]);
        if ((s_encoder_dev[i] == NULL) || (s_spi_dev[i] == NULL)) {
            ret = -1;
            continue;
        }

        if (s_encoder_dev[i]->init(&cfg) != 0) {
            ret = -1;
            continue;
        }
        if (s_encoder_dev[i]->get_info(&info) == 0) {
            if ((info.resolution_bits > 0U) && (info.resolution_bits <= 31U)) {
                float counts = (float)(1UL << info.resolution_bits);

                s_rad_scale[i] = 6.283185307179586f / counts;
                if (i == (uint8_t)APP_ENCODER_ROTOR) {
                    s_rotor_jump_limit = (int32_t)(counts * (APP_ENCODER_JUMP_LIMIT_DEG / 360.0f));
                }
                s_deg_scale[i] = 360.0f / counts;
            }
        }
    }

    /* 加载软件零点（flash 参数区；无有效记录时为 0） */
    if (app_param_init() == 0) {
        encoder_param_t param;

        memset(&param, 0, sizeof(param));
        if (app_param_load(APP_PARAM_KEY_ENCODER, &param, sizeof(param)) == 0) {
            s_param_loaded = true;
        }
        for (uint8_t i = 0U; i < (uint8_t)APP_ENCODER_COUNT; i++) {
            s_zero[i] = param.zero[i];
        }
    }

    return ret;
}

int app_encoder_read_raw(app_encoder_id_t id, uint16_t* raw) {
    if ((id >= APP_ENCODER_COUNT) || (raw == NULL) || (s_encoder_dev[id] == NULL)) {
        return -1;
    }
    return s_encoder_dev[id]->read_raw(raw);
}

/* 转子共享采样缓存（25kHz 单次读取；FOC 与 Debug 共用）
 * 注：单上下文（主循环）使用；若将来迁入 ISR，需重新审视本缓存的一致性。 */
static uint16_t s_rotor_raw;
static bool s_rotor_valid;
static uint32_t s_rotor_seq;      /* 成功采样序号（陈旧检测） */

int app_encoder_sample_rotor(void) {
    int rc = app_encoder_read_raw(APP_ENCODER_ROTOR, &s_rotor_raw);

    if (rc == 0) {
        /* 跳变防护：KTH7823 的 SPI 帧无 CRC/奇偶校验（驱动仅判 SPI 传输成败），
         * 坏帧会直接给出错误角度。25kHz 采样下真实机械角单步变化远小于阈值
         * （5° 对应 ~20000 rpm），超限即判为坏帧：丢弃本样本并计错误，
         * 避免坏帧把 N 倍电角误差注入 FOC 换相与辨识。 */
        if (s_rotor_prev_valid) {
            int32_t delta = (int32_t)s_rotor_raw - (int32_t)s_rotor_prev_raw;

            if (delta > 32767) {
                delta -= 65536;
            } else if (delta < -32768) {
                delta += 65536;
            }
            if ((delta > s_rotor_jump_limit) || (delta < -s_rotor_jump_limit)) {
                /* 坏帧：保持上一有效角（"采样保持"），序号照常推进 —— 若丢样本，
                 * FOC 的采样序号门控会判停摆 → 42% 拍走保护零矢量（台架实测），
                 * 比"角度短暂保持"更糟。计数供观测/辨识健康检查。 */
                s_rotor_jump_count++;
                s_rotor_seq++;
                s_rotor_valid = true;
                return 0;
            }
        }
        s_rotor_prev_raw = s_rotor_raw;
        s_rotor_prev_valid = true;
        s_rotor_seq++;
    }

    s_rotor_valid = (rc == 0);
    return rc;
}

int app_encoder_get_rotor_rad(float* rad, bool* valid, uint32_t* seq) {
    uint16_t raw;

    if (rad == NULL) {
        return -1;
    }
    if (app_encoder_get_rotor_raw(&raw, valid, seq) != 0) {
        return -1;
    }
    *rad = (float)raw * s_rad_scale[APP_ENCODER_ROTOR];
    return 0;
}

int app_encoder_get_rotor_raw(uint16_t* raw, bool* valid, uint32_t* seq) {
    if ((raw == NULL) || (valid == NULL)) {
        return -1;
    }
    *raw = s_rotor_raw;
    *valid = s_rotor_valid;
    if (seq != NULL) {
        *seq = s_rotor_seq;
    }
    return 0;
}

int app_encoder_read_position(app_encoder_id_t id, uint16_t* pos) {
    uint16_t raw;

    if ((id >= APP_ENCODER_COUNT) || (pos == NULL)) {
        return -1;
    }
    if (app_encoder_read_raw(id, &raw) != 0) {
        return -1;
    }

    *pos = (uint16_t)(raw - s_zero[id]);
    return 0;
}

int app_encoder_read_rad(app_encoder_id_t id, float* rad) {
    uint16_t pos;

    if ((id >= APP_ENCODER_COUNT) || (rad == NULL)) {
        return -1;
    }
    if (app_encoder_read_position(id, &pos) != 0) {
        return -1;
    }

    *rad = (float)pos * s_rad_scale[id];
    return 0;
}

int app_encoder_read_deg(app_encoder_id_t id, float* deg) {
    uint16_t pos;

    if ((id >= APP_ENCODER_COUNT) || (deg == NULL)) {
        return -1;
    }
    if (app_encoder_read_position(id, &pos) != 0) {
        return -1;
    }

    *deg = (float)pos * s_deg_scale[id];
    return 0;
}

int app_encoder_read_reg(app_encoder_id_t id, uint8_t addr, uint8_t* val) {
    if ((id >= APP_ENCODER_COUNT) || (val == NULL) || (s_encoder_dev[id] == NULL)) {
        return -1;
    }
    return s_encoder_dev[id]->read_reg(addr, val);
}

int app_encoder_set_zero(app_encoder_id_t id) {
    encoder_param_t param;
    uint16_t raw;

    if ((id >= APP_ENCODER_COUNT) || (s_encoder_dev[id] == NULL)) {
        return -1;
    }
    if (app_encoder_read_raw(id, &raw) != 0) {
        return -1;
    }

    memset(&param, 0, sizeof(param));
    (void)app_param_load(APP_PARAM_KEY_ENCODER, &param, sizeof(param));
    param.zero[id] = raw;
    if (app_param_store(APP_PARAM_KEY_ENCODER, &param, sizeof(param)) != 0) {
        return -1;
    }

    s_zero[id] = raw;
    s_param_loaded = true;
    return 0;
}

int app_encoder_clear_zero(app_encoder_id_t id) {
    encoder_param_t param;

    if (id >= APP_ENCODER_COUNT) {
        return -1;
    }

    memset(&param, 0, sizeof(param));
    (void)app_param_load(APP_PARAM_KEY_ENCODER, &param, sizeof(param));
    param.zero[id] = 0U;
    if (app_param_store(APP_PARAM_KEY_ENCODER, &param, sizeof(param)) != 0) {
        return -1;
    }

    s_zero[id] = 0U;
    return 0;
}

int app_encoder_get_zero(app_encoder_id_t id, uint16_t* zero) {
    if ((id >= APP_ENCODER_COUNT) || (zero == NULL)) {
        return -1;
    }
    *zero = s_zero[id];
    return 0;
}

bool app_encoder_is_param_loaded(void) { return s_param_loaded; }

int app_encoder_set_zero_mtp(app_encoder_id_t id, uint16_t zero) {
    if ((id >= APP_ENCODER_COUNT) || (s_encoder_dev[id] == NULL)) {
        return -1;
    }
    return s_encoder_dev[id]->set_zero(zero);
}

int app_encoder_set_direction(app_encoder_id_t id, bool cw_increasing) {
    if ((id >= APP_ENCODER_COUNT) || (s_encoder_dev[id] == NULL)) {
        return -1;
    }
    return s_encoder_dev[id]->set_direction(cw_increasing);
}

uint32_t app_encoder_get_rotor_jump_count(void) {
    return s_rotor_jump_count;
}

uint32_t app_encoder_get_error_count(app_encoder_id_t id) {
    if ((id >= APP_ENCODER_COUNT) || (s_encoder_dev[id] == NULL)) {
        return 0U;
    }
    return s_encoder_dev[id]->get_error_count();
}

uint32_t app_encoder_get_sclk_hz(app_encoder_id_t id) {
    if ((id >= APP_ENCODER_COUNT) || (s_spi_dev[id] == NULL)) {
        return 0U;
    }
    return s_spi_dev[id]->get_sclk_hz();
}
