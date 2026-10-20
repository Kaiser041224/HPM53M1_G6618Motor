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
 * 实时性：
 *   - 启动/自检期：read_raw 为阻塞短操作（两帧 ~7µs），仅调度器前后的初始化阶段使用；
 *   - 运行期（app_encoder_sync_start 之后）：转子 SPI3 由硬件同步采样接管——
 *     PWM1 CMP10（与相电流 ADC 同一触发点）→ TRGM DMA_REQ0 → HDMA 写 SPI3 CMD 启动单帧，
 *     角度与电流同拍采样；ADC0 PMT 完成中断（FOC 快车道）内 app_encoder_sync_sample()
 *     取SPI3及SPI1 RX字并发布完整配对（游标亦为PWM30kHz触发，DMA_REQ1/HDMA1）。
 *   - 单一所有者：ADC ISR取两路帧；运行期读API只返回快照，绝不重新触碰SPI。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_encoder.h"

#include "algo_encoder_snapshot.h"
#include "app_param.h"
#include "app_ramfunc.h"
#include "intf_clock.h"
#include "intf_encoder.h"
#include "intf_spi.h"
#include "intf_sys.h"
#include "intf_trgm.h"

#include <string.h>

#define APP_ENCODER_SCLK_HZ \
    (8000000U) /* 8MHz：手册≤10MHz；10MHz 台架实测坏帧率高（EMI），取 8MHz */

/** 机械角单步跳变上限 [deg]（30kHz 采样；5° ≈ 25000 rpm，远超实际转速） */
#define APP_ENCODER_JUMP_LIMIT_DEG (5.0f)

/* 同步采样触发：与相电流 ADC 共用 PWM1 CMP10（app_adc.h APP_ADC_TRIGGER_CMP_INDEX），
 * 经 TRGM DMA 请求 0 驱动 HDMA 启动 SPI3 单帧 —— 角度与电流同一时刻采样。 */
#define APP_ENCODER_SYNC_TRGM_SRC INTF_TRGM_SRC_PWM1_CH10REF
#define APP_ENCODER_SYNC_TRGM_DST INTF_TRGM_DST_DMA_REQ0
#define APP_ENCODER_SYNC_SPI_TRIG INTF_SPI_TRIG_DMA_REQ0
/* 取帧时帧仍在传输的最长等待 [µs]：16bit@8MHz = 2µs + cs2sclk + csht(12 半 SCLK = 0.75µs)
 * ≈ 3µs。正常 ISR 在 ADC 转换（~4µs）之后进入，帧已结束、不等待；仅 ISR 迟到一个
 * 周期（下一帧正在传输）时才等待，8µs 保证该情形能取到完整的新帧而非判失败。 */
#define APP_ENCODER_SYNC_WAIT_US (8U)

/**
 * @brief 编码器持久化参数（app_param 键 APP_PARAM_KEY_ENCODER 的数据布局）
 */
typedef struct {
    uint16_t zero[APP_ENCODER_COUNT]; /**< 软件零点（原始值） */
    uint16_t flags;                   /**< 预留（方向等） */
} encoder_param_t;

/* 板级映射：转子 -> SPI3，出轴 -> SPI1（SoC 实例号） */
static algo_encoder_snapshot_t
    s_rotor_snap;                  /**< 转子一致快照（唯一写者 = ADC0 PMT ISR 同步采样） */
static int32_t s_rotor_jump_limit; /**< 单步跳变上限 [count]（init 时按分辨率算） */
static volatile bool s_rotor_sampler_claimed; /**< 运行期 SPI3 所有权已移交采样器 */
static volatile bool s_rotor_sync_armed;      /**< 硬件同步采样已启用（ISR 门控，最后发布） */
static uint32_t s_rotor_sync_wait_cycles;     /**< 取帧最长等待 [cycle]（start 时换算） */
/** 同步取帧函数（start 时缓存到 RAM，热路径免读 flash 中的设备对象） */
static int (*s_rotor_sync_fetch)(uint16_t* raw, uint32_t wait_cycles);
static volatile bool s_output_sync_armed; /**< SPI1归ADC侧同步采样器独占 */
static uint16_t s_output_raw;             /**< 最近成功取到的游标原码，独立诊断 */
static bool s_output_cache_valid;
static int (*s_output_fetch)(uint16_t*, uint32_t);
static uint32_t s_output_max_age_cycles;
/** @brief 同一ADC回调发布的双编码器快照；任何一路失败即配对无效 */
static struct {
    app_encoder_rotor_snapshot_t rotor;
    uint16_t output_raw;
    bool valid;
} s_encoder_pair;
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
    s_rotor_sync_armed = false;
    s_rotor_sync_fetch = NULL;
    s_output_sync_armed = false;
    s_output_fetch = NULL;
    s_output_cache_valid = false;
    s_output_raw = 0U;
    s_encoder_pair.valid = false;

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
volatile uint32_t g_encoder_output_sample_count __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_encoder_output_read_fail_count __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_encoder_pair_valid_count __attribute__((section(".noncacheable.bss")));

int app_encoder_sync_start(void) {
    const intf_encoder_t* dev = s_encoder_dev[APP_ENCODER_ROTOR];

    if (s_rotor_sync_armed) {
        return 0; /* 幂等 */
    }
    if ((dev == NULL) || (dev->sync_arm == NULL) || (dev->sync_fetch == NULL)) {
        return -1;
    }
    s_rotor_sync_wait_cycles = (intf_clock_get_cpu_freq() / 1000000U) * APP_ENCODER_SYNC_WAIT_US;

    /* 先 arm（TX FIFO 预填 + DMA 就绪），再接通触发路由：首个触发即为完整帧 */
    app_encoder_sampler_claim();
    if (dev->sync_arm((uint8_t)APP_ENCODER_SYNC_SPI_TRIG) != 0) {
        app_encoder_sampler_release_claim();
        return -1;
    }
    if (intf_trgm_connect(APP_ENCODER_SYNC_TRGM_SRC, APP_ENCODER_SYNC_TRGM_DST) != 0) {
        if (dev->sync_disarm != NULL) {
            dev->sync_disarm();
        }
        app_encoder_sampler_release_claim();
        return -1;
    }
    s_rotor_sync_fetch = dev->sync_fetch;
    s_rotor_sync_armed = true;
    return 0;
}

bool app_encoder_sync_active(void) { return s_rotor_sync_armed; }

int app_encoder_output_start(void) {
    const intf_encoder_t* dev = s_encoder_dev[APP_ENCODER_OUTPUT];
    if (s_output_sync_armed)
        return 0;
    if (!dev || !dev->sync_arm || !dev->sync_fetch)
        return -1;
    if (dev->sync_arm(INTF_SPI_TRIG_DMA_REQ1) != 0)
        return -1;
    if (intf_trgm_connect(APP_ENCODER_SYNC_TRGM_SRC, INTF_TRGM_DST_DMA_REQ1) != 0) {
        if (dev->sync_disarm)
            dev->sync_disarm();
        return -1;
    }
    s_output_fetch = dev->sync_fetch;
    /* 快照超龄与原有转子守卫一致；这不是额外SPI等待时间。 */
    s_output_max_age_cycles = intf_clock_get_cpu_freq() / 25000U;
    s_encoder_pair.valid = false;
    s_output_sync_armed = true;
    return 0;
}
APP_ATTR_RAMFUNC
int app_encoder_output_sample(app_encoder_rotor_snapshot_t* rotor, uint16_t* output) {
    if (!rotor || !output || !s_output_sync_armed)
        return -1;
    /* 唯一写者ADC ISR；读者仅短临界区复制，不访问SPI，不自旋等待发布。 */
    uint32_t state = intf_sys_irq_save();
    *rotor = s_encoder_pair.rotor;
    *output = s_encoder_pair.output_raw;
    bool valid = s_encoder_pair.valid;
    intf_sys_irq_restore(state);
    rotor->age_cycles = intf_clock_get_cycle() - rotor->timestamp_cycles;
    return valid && rotor->age_cycles <= s_output_max_age_cycles ? 0 : -1;
}
int app_encoder_get_output_raw(uint16_t* raw) {
    if (raw == NULL || !s_output_cache_valid)
        return -1;
    *raw = s_output_raw;
    return 0;
}
void app_encoder_get_pair_stats(app_encoder_pair_stats_t* out) {
    if(out!=NULL) {
        uint32_t state=intf_sys_irq_save();
        out->output_sample_count=g_encoder_output_sample_count;
        out->output_fail_count=g_encoder_output_read_fail_count;
        out->valid_pair_count=g_encoder_pair_valid_count;
        intf_sys_irq_restore(state);
    }
}

int app_encoder_read_raw(app_encoder_id_t id, uint16_t* raw) {
    if ((id >= APP_ENCODER_COUNT) || (raw == NULL)) {
        return -1;
    }
    if (id == APP_ENCODER_OUTPUT && s_output_sync_armed) {
        return app_encoder_get_output_raw(raw);
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

/**
 * @brief 快照样本 → 平台快照（含按器件分辨率换算的机械角）
 * @param sample 算法层样本
 * @param out 平台快照输出
 */
APP_ATTR_RAMFUNC
static void app_encoder_fill_snapshot(
    const algo_encoder_sample_t* sample, app_encoder_rotor_snapshot_t* out) {
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

APP_ATTR_RAMFUNC
int app_encoder_sync_sample(uint32_t now_cycles, app_encoder_rotor_snapshot_t* out) {
    algo_encoder_input_t in = {
        .raw = 0U,
        .ok = false,
        .timestamp_cycles = now_cycles,
    };
    uint32_t t0;
    uint32_t cycles;
    int rc;

    if (!s_rotor_sync_armed) {
        algo_encoder_sample_t sample = s_rotor_snap.sample;

        app_encoder_fill_snapshot(&sample, out);
        out->valid = false;
        return -1;
    }

    t0 = intf_clock_get_cycle();
    /* 唯一运行期 SPI3 读点：取硬件触发帧（正常已完成，无等待） */
    if (s_rotor_sync_fetch(&in.raw, s_rotor_sync_wait_cycles) == 0) {
        in.ok = true;
    } else {
        g_encoder_read_fail_count++;
    }
    rc = algo_encoder_snapshot_push(&s_rotor_snap, &in);

    /* 写者即读者（同一 ISR 上下文）：发布值直接取用，age = 0 */
    app_encoder_fill_snapshot(&s_rotor_snap.sample, out);
    out->age_cycles = 0U;

    /* 两路由同一个PWM边沿独立触发，ADC每拍排空SPI1并补FIFO。
     * 转子电流路径不消费游标，仅发布给GPTMR外环；SPI1失败不改变转子rc。 */
    if (s_output_sync_armed) {
        uint16_t output_raw = 0U;
        bool output_ok = s_output_fetch(&output_raw, 0U) == 0;
        g_encoder_output_sample_count++;
        if (output_ok) {
            s_output_raw = output_raw;
            s_output_cache_valid = true;
        } else {
            g_encoder_output_read_fail_count++;
        }
        s_encoder_pair.rotor = *out;
        s_encoder_pair.output_raw = output_raw;
        s_encoder_pair.valid = output_ok && rc == ALGO_ENC_PUSH_ACCEPTED;
        if (s_encoder_pair.valid)
            g_encoder_pair_valid_count++;
    }

    cycles = intf_clock_get_cycle() - t0;
    g_encoder_isr_cycles = cycles;
    if (cycles > g_encoder_isr_cycles_max) {
        g_encoder_isr_cycles_max = cycles;
    }
    g_encoder_sample_count++;

    return (rc == ALGO_ENC_PUSH_ACCEPTED) ? 0 : ((rc == ALGO_ENC_PUSH_HELD) ? 1 : -1);
}

APP_ATTR_RAMFUNC
int app_encoder_get_rotor_snapshot(app_encoder_rotor_snapshot_t* out) {
    algo_encoder_sample_t sample;
    bool coherent;

    if (out == NULL) {
        return -1;
    }
    /* 任务上下文读者：8 次有界重试（写者 = ADC0 ISR 同步采样，可在读期间发布） */
    coherent = algo_encoder_snapshot_read(&s_rotor_snap, intf_clock_get_cycle(), &sample, 8U);
    app_encoder_fill_snapshot(&sample, out);
    if (!coherent) {
        out->valid = false; /* 未取到一致快照：消费方不得使用 */
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

uint32_t app_encoder_get_rotor_jump_count(void) { return s_rotor_snap.jump_count; }

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
