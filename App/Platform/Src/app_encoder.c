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

#include "algo_encoder_snapshot.h"
#include "app_gptmr.h"
#include "app_param.h"
#include "intf_clock.h"
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
static algo_encoder_snapshot_t s_rotor_snap; /**< 转子一致快照（唯一写者 = 采样 ISR） */
static int32_t s_rotor_jump_limit;           /**< 单步跳变上限 [count]（init 时按分辨率算） */
static volatile bool s_rotor_sampler_claimed; /**< 运行期 SPI3 所有权已移交采样器 */
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

    /* 转子快照构造（跳变上限按器件分辨率算出；连续失败上限默认 3） */
    {
        algo_encoder_snapshot_cfg_t snap_cfg = {
            .jump_limit_counts = s_rotor_jump_limit,
            .fail_limit = ALGO_ENCODER_FAIL_LIMIT_DEFAULT,
            .stale_cycles = 0U, /* 时效由消费方按节拍判定（见 app_foc_isr_step） */
        };

        algo_encoder_snapshot_ctor(&s_rotor_snap, &snap_cfg);
    }
    s_rotor_sampler_claimed = false;

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

void app_encoder_sampler_claim(void) { s_rotor_sampler_claimed = true; }

void app_encoder_sampler_release_claim(void) { s_rotor_sampler_claimed = false; }

bool app_encoder_sampler_active(void) { return s_rotor_sampler_claimed; }

/* 采样器观测（.noncacheable.bss） */
volatile uint32_t g_encoder_isr_cycles __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_encoder_isr_cycles_max __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_encoder_sample_count __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_encoder_read_fail_count __attribute__((section(".noncacheable.bss")));

/**
 * @brief GPTMR1 CH3 中断回调：采样转子（唯一物理 SPI3 运行期读点）
 */
static void app_encoder_sample_rotor_isr(void) { (void)app_encoder_sample_rotor(); }

int app_encoder_sampler_start(void) {
    int rc;

    if (s_encoder_dev[APP_ENCODER_ROTOR] == NULL) {
        return -1;
    }
    app_encoder_sampler_claim();
    rc = app_gptmr_register_callback(APP_GPTMR_CH_3, app_encoder_sample_rotor_isr);
    if (rc != 0) {
        app_encoder_sampler_release_claim();
        return -1;
    }
    rc = app_gptmr_start(APP_GPTMR_CH_3);
    if (rc != 0) {
        app_encoder_sampler_release_claim();
        return -1;
    }
    return 0;
}

int app_encoder_read_raw(app_encoder_id_t id, uint16_t* raw) {
    if ((id >= APP_ENCODER_COUNT) || (raw == NULL)) {
        return -1;
    }
    /* 转子在采样器声明所有权后：只读快照，绝不触碰物理 SPI（单一所有者） */
    if ((id == APP_ENCODER_ROTOR) && s_rotor_sampler_claimed) {
        app_encoder_rotor_snapshot_t snap;

        if ((app_encoder_get_rotor_snapshot(&snap) != 0) || !snap.valid) {
            return -1;
        }
        *raw = snap.raw;
        return 0;
    }
    if (s_encoder_dev[id] == NULL) {
        return -1;
    }
    return s_encoder_dev[id]->read_raw(raw);
}

int app_encoder_sample_rotor_at(uint32_t now_cycles) {
    algo_encoder_input_t in = {
        .raw = 0U,
        .ok = false,
        .timestamp_cycles = now_cycles,
    };
    uint32_t t0 = intf_clock_get_cycle();
    uint32_t cycles;
    int rc;

    if (s_encoder_dev[APP_ENCODER_ROTOR] == NULL) {
        return -1;
    }
    /* 唯一物理 SPI3 读点（运行期）：设备读失败按无效样本处理 */
    if (s_encoder_dev[APP_ENCODER_ROTOR]->read_raw(&in.raw) == 0) {
        in.ok = true;
    } else {
        g_encoder_read_fail_count++;
    }
    rc = algo_encoder_snapshot_push(&s_rotor_snap, &in);

    cycles = intf_clock_get_cycle() - t0;
    g_encoder_isr_cycles = cycles;
    if (cycles > g_encoder_isr_cycles_max) {
        g_encoder_isr_cycles_max = cycles;
    }
    g_encoder_sample_count++;

    return (rc == ALGO_ENC_PUSH_ACCEPTED) ? 0 : ((rc == ALGO_ENC_PUSH_HELD) ? 1 : -1);
}

int app_encoder_sample_rotor(void) { return app_encoder_sample_rotor_at(intf_clock_get_cycle()); }

static void app_encoder_fill_snapshot(const algo_encoder_sample_t* sample,
                                      app_encoder_rotor_snapshot_t* out) {
    out->raw = sample->raw;
    out->rad = (float)sample->raw * s_rad_scale[APP_ENCODER_ROTOR];
    out->seq = sample->seq;
    out->timestamp_cycles = sample->timestamp_cycles;
    out->age_cycles = sample->age_cycles;
    out->consecutive_fail = sample->consecutive_fail;
    out->valid = sample->valid;
    out->jumped = sample->jumped;
    out->read_failed = sample->read_failed;
}

int app_encoder_get_rotor_snapshot(app_encoder_rotor_snapshot_t* out) {
    algo_encoder_sample_t sample;
    bool coherent;

    if (out == NULL) {
        return -1;
    }
    /* 主循环读者允许 8 次有界重试；更高优先级 ISR 读者用 read_rotor_isr */
    coherent = algo_encoder_snapshot_read(&s_rotor_snap, intf_clock_get_cycle(), &sample, 8U);
    app_encoder_fill_snapshot(&sample, out);
    if (!coherent) {
        out->valid = false; /* 未取到一致快照：消费方不得使用 */
    }
    return 0;
}

int app_encoder_read_rotor_isr(app_encoder_rotor_snapshot_t* out) {
    algo_encoder_sample_t sample;
    bool coherent;

    if (out == NULL) {
        return -1;
    }
    /* ISR：最多 1 次重试，取不到也返回当前值（不阻塞） */
    coherent = algo_encoder_snapshot_read(&s_rotor_snap, intf_clock_get_cycle(), &sample, 1U);
    app_encoder_fill_snapshot(&sample, out);
    if (!coherent) {
        out->valid = false;
    }
    return 0;
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
    app_encoder_rotor_snapshot_t sample;

    if ((raw == NULL) || (valid == NULL)) {
        return -1;
    }
    if (app_encoder_get_rotor_snapshot(&sample) != 0) {
        return -1;
    }
    *raw = sample.raw;
    *valid = sample.valid;
    if (seq != NULL) {
        *seq = sample.seq;
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
    /* 转子 SPI3 在采样器声明所有权后独占：运行期寄存器读不得触碰物理总线 */
    if ((id == APP_ENCODER_ROTOR) && s_rotor_sampler_claimed) {
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
    /* 运行期 MTP 写被禁止（采样器独占 SPI3；且 MTP 寿命有限） */
    if ((id == APP_ENCODER_ROTOR) && s_rotor_sampler_claimed) {
        return -1;
    }
    return s_encoder_dev[id]->set_zero(zero);
}

int app_encoder_set_direction(app_encoder_id_t id, bool cw_increasing) {
    if ((id >= APP_ENCODER_COUNT) || (s_encoder_dev[id] == NULL)) {
        return -1;
    }
    /* 运行期方向写被禁止（采样器独占 SPI3；方向应在停机标定期设置） */
    if ((id == APP_ENCODER_ROTOR) && s_rotor_sampler_claimed) {
        return -1;
    }
    return s_encoder_dev[id]->set_direction(cw_increasing);
}

uint32_t app_encoder_get_rotor_jump_count(void) {
    return s_rotor_snap.jump_count;
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
