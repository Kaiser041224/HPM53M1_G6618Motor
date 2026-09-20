/*
 * App Fault - 故障保护与错误处理（v1：纯判断）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 三级过流保护 + 链路健康；只记录/输出，不执行任何动作（v2 接入动作层）。
 * 阈值默认值见 app_fault.h（2026-09-19 评审确认）。
 */

#include "app_fault.h"

#include "algo_rms.h"
#include "app_analog_signal.h"
#include "app_encoder.h"
#include "intf_sys.h"

#include <stddef.h>
#include <string.h>

/* Ozone 观测变量（.noncacheable.bss：调试器直读） */
volatile uint32_t g_fault_state __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_fault_codes __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_fault_latched __attribute__((section(".noncacheable.bss")));

#define APP_FAULT_EVENT_COUNT (10U) /* bit0..9 事件计数 */
#define APP_FAULT_OC_COUNT    (3U)

typedef struct {
    /* 配置（已解析默认值） */
    float    oc_fast_a;
    float    oc_slow_a;
    float    vbus_ov_v;
    float    vbus_uv_v;
    uint16_t slow_debounce;
    uint16_t adc_stall_ms;
    uint8_t  enc_err_delta;
    uint16_t wdog_thshd_high;
    uint16_t wdog_thshd_low;

    /* 状态 */
    app_fault_state_t state;
    uint32_t active;  /* 当前存在的条件 */
    uint32_t latched; /* 锁存故障 */
    uint32_t first;   /* 首故障码 */
    uint16_t event_cnt[APP_FAULT_EVENT_COUNT];

    /* 去抖 / 健康 */
    uint16_t oc_slow_cnt[APP_FAULT_OC_COUNT];
    uint16_t vbus_ov_cnt;
    uint16_t vbus_uv_cnt;
    uint16_t adc_stall_cnt;
    uint16_t settle_ticks; /* 上电静默期计数 */
    uint32_t enc_err_last;
    uint32_t seq_last;

    app_fault_snapshot_t snap;
} app_fault_ctx_t;

static app_fault_ctx_t s_f;
static bool s_ready;

/* RMS：三相真有效值（10ms 滑窗 @25kHz） */
static algo_rms_t s_rms[APP_FAULT_OC_COUNT];
static float s_rms_buf[APP_FAULT_OC_COUNT][APP_FAULT_RMS_WINDOW_DEFAULT];

/* -------------------------------------------------------------------------- */

static void fault_publish(void) {
    g_fault_state = (uint32_t) s_f.state;
    g_fault_codes = s_f.active;
    g_fault_latched = s_f.latched;
}

static void fault_snapshot_now(void) {
    app_analog_values_t v;

    if (app_analog_signal_read_all(&v)) {
        s_f.snap.i_u_a = v.i_u_a;
        s_f.snap.i_v_a = v.i_v_a;
        s_f.snap.i_w_a = v.i_w_a;
        s_f.snap.v_bus_v = v.v_bus_v;
    }
}

/* 置位故障：更新有效/锁存/首故障/事件计数/快照/状态。
 * 已锁存的故障只维持 active（不重复计数/不覆盖首故障快照）。 */
static void fault_raise(uint32_t code, uint16_t oc_fast_raw) {
    uint8_t bit;

    s_f.active |= code;
    fault_publish(); /* 即使已锁存也同步 active 镜像（Ozone 观测变量） */

    if ((s_f.latched & code) != 0U) {
        return; /* 已锁存：仅维持 active */
    }
    s_f.latched |= code;

    for (bit = 0U; bit < APP_FAULT_EVENT_COUNT; bit++) {
        if ((code & (1UL << bit)) != 0U) {
            if (s_f.event_cnt[bit] < 0xFFFFU) {
                s_f.event_cnt[bit]++;
            }
        }
    }

    if (s_f.first == 0U) {
        s_f.first = code;
        s_f.snap.code = code;
        s_f.snap.oc_fast_raw = oc_fast_raw;
        fault_snapshot_now();
    }

    s_f.state = APP_FAULT_STATE_FAULT;
    fault_publish();
}

/* 条件复检：L2/L3 用当前值 + L1 用最新原始值（主循环与 tick 共用） */
static bool fault_conditions_ok(void) {
    app_analog_values_t v;

    if (app_analog_signal_read_all(&v)) {
        if ((v.v_bus_v > s_f.vbus_ov_v) || (v.v_bus_v < s_f.vbus_uv_v)) {
            return false;
        }
    }
    for (uint8_t i = 0U; i < APP_FAULT_OC_COUNT; i++) {
        if (s_rms[i].get_rms(&s_rms[i]) > s_f.oc_slow_a) {
            return false;
        }
    }
    for (uint8_t ch = (uint8_t) ADC_CH_I_U; ch <= (uint8_t) ADC_CH_I_W; ch++) {
        uint16_t raw = 0U;

        if (!app_adc_get_raw((adc_channel_t) ch, &raw)) {
            return false; /* 无数据视为条件未确认 */
        }
        if ((raw > s_f.wdog_thshd_high) || (raw < s_f.wdog_thshd_low)) {
            return false;
        }
    }
    return true;
}

/* 复位故障状态与去抖计数（清除路径；须在 ISR 上下文调用）。
 * 语义：F = 完整复位——状态/锁存/首故障/快照/事件计数全部归零。 */
static void fault_reset_all(void) {
    s_f.latched = 0U;
    s_f.active = 0U;
    s_f.first = 0U;
    memset(&s_f.snap, 0, sizeof(s_f.snap));
    memset(s_f.event_cnt, 0, sizeof(s_f.event_cnt));
    memset(s_f.oc_slow_cnt, 0, sizeof(s_f.oc_slow_cnt));
    s_f.vbus_ov_cnt = 0U;
    s_f.vbus_uv_cnt = 0U;
    s_f.adc_stall_cnt = 0U;
    /* 编码器错误计数重新基线（自清除起重新累计） */
    s_f.enc_err_last = app_encoder_get_error_count(APP_ENCODER_ROTOR)
                     + app_encoder_get_error_count(APP_ENCODER_OUTPUT);
    s_f.state = APP_FAULT_STATE_NORMAL;
    fault_publish();

    /* 重装 L1 WDOG 中断（ISR 命中后驱动自动关闭该通道） */
    for (uint8_t ch = (uint8_t) ADC_CH_I_U; ch <= (uint8_t) ADC_CH_I_W; ch++) {
        app_adc_wdog_reenable((adc_channel_t) ch);
    }
}

/* -------------------------------------------------------------------------- */

void app_fault_init(const app_fault_cfg_t *cfg) {
    algo_rms_cfg_t rms_cfg;
    float dev_v;
    float dev_cnt;
    uint32_t dev;
    uint32_t mid;

    memset(&s_f, 0, sizeof(s_f));

    s_f.oc_fast_a = ((cfg != NULL) && (cfg->oc_fast_a > 0.0f))
                        ? cfg->oc_fast_a
                        : APP_FAULT_OC_TRIP_A_DEFAULT;
    s_f.oc_slow_a = ((cfg != NULL) && (cfg->oc_slow_a > 0.0f))
                        ? cfg->oc_slow_a
                        : APP_FAULT_OC_TRIP_A_DEFAULT;
    s_f.vbus_ov_v = ((cfg != NULL) && (cfg->vbus_ov_v > 0.0f))
                        ? cfg->vbus_ov_v
                        : APP_FAULT_VBUS_OV_V_DEFAULT;
    s_f.vbus_uv_v = ((cfg != NULL) && (cfg->vbus_uv_v > 0.0f))
                        ? cfg->vbus_uv_v
                        : APP_FAULT_VBUS_UV_V_DEFAULT;
    s_f.slow_debounce = ((cfg != NULL) && (cfg->slow_debounce > 0U))
                            ? cfg->slow_debounce
                            : APP_FAULT_SLOW_DEBOUNCE_DEFAULT;
    s_f.adc_stall_ms = ((cfg != NULL) && (cfg->adc_stall_ms > 0U))
                           ? cfg->adc_stall_ms
                           : APP_FAULT_ADC_STALL_MS_DEFAULT;
    s_f.enc_err_delta = ((cfg != NULL) && (cfg->enc_err_delta > 0U))
                            ? cfg->enc_err_delta
                            : APP_FAULT_ENC_ERR_DELTA_DEFAULT;

    /* WDOG 原始窗口：中值 ± (阈值[A] / 转换[A/V] → 电压 → 码值)
     * 注1：基于标称零点（半量程）；逐相标定后重装为 v2 精化项
     * 注2：65535 = 16bit 满量程（与 app_adc 默认分辨率一致；改分辨率需同步） */
    dev_v = s_f.oc_fast_a / APP_ANALOG_I_AMP_PER_VOLT;
    dev_cnt = dev_v * (65535.0f / (INTF_ADC_DEFAULT_VREF_MV / 1000.0f));
    dev = (uint32_t) (dev_cnt + 0.5f);
    mid = 65535U / 2U;
    s_f.wdog_thshd_high = (uint16_t) (((mid + dev) > 65535U) ? 65535U : (mid + dev));
    s_f.wdog_thshd_low = (uint16_t) ((dev > mid) ? 0U : (mid - dev));

    /* RMS 初始化（真有效值，不去直流）：
     * 必须先 ctor（填充 ops 函数指针 + 复位内部状态），再 init —— 否则 ops 为 NULL */
    memset(&rms_cfg, 0, sizeof(rms_cfg));
    rms_cfg.window_size = APP_FAULT_RMS_WINDOW_DEFAULT;
    rms_cfg.remove_dc = false;
    for (uint8_t i = 0U; i < APP_FAULT_OC_COUNT; i++) {
        algo_rms_ctor(&s_rms[i]);
        rms_cfg.buffer = s_rms_buf[i];
        (void) s_rms[i].init(&s_rms[i], &rms_cfg);
    }

    s_f.enc_err_last = app_encoder_get_error_count(APP_ENCODER_ROTOR)
                     + app_encoder_get_error_count(APP_ENCODER_OUTPUT);
    s_f.seq_last = app_adc_get_sequence();
    s_f.state = APP_FAULT_STATE_NORMAL;
    s_ready = true;
    fault_publish();
}

void app_fault_process(void) {
    app_analog_values_t v;

    if (!s_ready || !app_analog_signal_read_all(&v)) {
        return;
    }

    (void) s_rms[0].step(&s_rms[0], v.i_u_a);
    (void) s_rms[1].step(&s_rms[1], v.i_v_a);
    (void) s_rms[2].step(&s_rms[2], v.i_w_a);
}

void app_fault_tick(void) {
    uint32_t seq;
    float vbus;
    bool pending = false;

    if (!s_ready) {
        return;
    }

    /* 上电静默期：等待 ADC/模拟量/编码器链路稳定（避免启动瞬态误报） */
    if (s_f.settle_ticks < APP_FAULT_SETTLE_TICKS_DEFAULT) {
        s_f.settle_ticks++;
        s_f.seq_last = app_adc_get_sequence();
        return;
    }

    /* 驱动每帧回调一次（SEQ_CMPT） */
    seq = app_adc_get_sequence();

    /* ---- L2：RMS 判断（连续去抖） ---- */
    for (uint8_t i = 0U; i < APP_FAULT_OC_COUNT; i++) {
        float rms = s_rms[i].get_rms(&s_rms[i]);

        if (rms > s_f.oc_slow_a) {
            if (s_f.oc_slow_cnt[i] < 0xFFFFU) {
                s_f.oc_slow_cnt[i]++;
            }
        } else {
            s_f.oc_slow_cnt[i] = 0U;
        }

        if (s_f.oc_slow_cnt[i] >= s_f.slow_debounce) {
            fault_raise(APP_FAULT_OC_SLOW_U << i, 0U);
        } else if (s_f.oc_slow_cnt[i] > 0U) {
            pending = true;
        }
    }

    /* ---- L3：母线判断（连续去抖） ---- */
    vbus = app_analog_signal_read(ADC_CH_V_VBUS);
    if (vbus > s_f.vbus_ov_v) {
        if (s_f.vbus_ov_cnt < 0xFFFFU) {
            s_f.vbus_ov_cnt++;
        }
    } else {
        s_f.vbus_ov_cnt = 0U;
    }
    if (vbus < s_f.vbus_uv_v) {
        if (s_f.vbus_uv_cnt < 0xFFFFU) {
            s_f.vbus_uv_cnt++;
        }
    } else {
        s_f.vbus_uv_cnt = 0U;
    }
    if (s_f.vbus_ov_cnt >= s_f.slow_debounce) {
        fault_raise(APP_FAULT_VBUS_OV, 0U);
    } else if (s_f.vbus_ov_cnt > 0U) {
        pending = true;
    }
    if (s_f.vbus_uv_cnt >= s_f.slow_debounce) {
        fault_raise(APP_FAULT_VBUS_UV, 0U);
    } else if (s_f.vbus_uv_cnt > 0U) {
        pending = true;
    }

    /* ---- 健康：PMT 帧停滞 ---- */
    if (seq == s_f.seq_last) {
        if (s_f.adc_stall_cnt < 0xFFFFU) {
            s_f.adc_stall_cnt++;
        }
    } else {
        s_f.seq_last = seq;
        s_f.adc_stall_cnt = 0U;
    }
    if (s_f.adc_stall_cnt > s_f.adc_stall_ms) { /* >10ms：第 11 个无新帧 tick 触发 */
        fault_raise(APP_FAULT_ADC_TIMEOUT, 0U);
    }

    /* ---- 健康：编码器错误增量 ---- */
    {
        uint32_t ec = app_encoder_get_error_count(APP_ENCODER_ROTOR)
                    + app_encoder_get_error_count(APP_ENCODER_OUTPUT);

        if ((ec - s_f.enc_err_last) >= (uint32_t) s_f.enc_err_delta) {
            s_f.enc_err_last = ec;
            fault_raise(APP_FAULT_ENC_READ, 0U);
        }
    }

    /* ---- 条件恢复时清除 active（latched 保持锁存；ENC_READ 为事件型仅经清除解除） ---- */
    for (uint8_t i = 0U; i < APP_FAULT_OC_COUNT; i++) {
        if (s_f.oc_slow_cnt[i] == 0U) {
            s_f.active &= ~(APP_FAULT_OC_SLOW_U << i);
        }
    }
    if (s_f.vbus_ov_cnt == 0U) {
        s_f.active &= ~APP_FAULT_VBUS_OV;
    }
    if (s_f.vbus_uv_cnt == 0U) {
        s_f.active &= ~APP_FAULT_VBUS_UV;
    }
    if (s_f.adc_stall_cnt == 0U) {
        s_f.active &= ~APP_FAULT_ADC_TIMEOUT;
    }
    for (uint8_t ch = (uint8_t) ADC_CH_I_U; ch <= (uint8_t) ADC_CH_I_W; ch++) {
        uint16_t raw = 0U;

        if (app_adc_get_raw((adc_channel_t) ch, &raw)
            && (raw <= s_f.wdog_thshd_high) && (raw >= s_f.wdog_thshd_low)) {
            s_f.active &= ~(APP_FAULT_OC_FAST_U << (uint8_t) (ch - (uint8_t) ADC_CH_I_U));
        }
    }

    /* ---- 状态汇总（FAULT 锁存优先） ---- */
    if (s_f.latched != 0U) {
        s_f.state = APP_FAULT_STATE_FAULT;
    } else if (pending) {
        s_f.state = APP_FAULT_STATE_WARNING;
    } else {
        s_f.state = APP_FAULT_STATE_NORMAL;
    }
    fault_publish();
}

void app_fault_on_wdog(adc_channel_t ch, uint16_t value, void *user) {
    (void) user;

    if (!s_ready || (ch < ADC_CH_I_U) || (ch > ADC_CH_I_W)) {
        return;
    }
    fault_raise(APP_FAULT_OC_FAST_U << (uint8_t) (ch - ADC_CH_I_U), value);
}

int app_fault_clear(void) {
    uint32_t mstatus;
    bool ok;

    if (!s_ready) {
        return -1;
    }

    /* 临界区：与 ADC0/ADC1 故障 ISR 互斥。注意 ADC0 WDOG 优先级高于 ADC1 且
     * SDK ISR 允许嵌套——不关中断时 WDOG 事件可在"复检 → 复位"之间插入并被擦除。 */
    mstatus = intf_sys_irq_save();
    ok = fault_conditions_ok();
    if (ok) {
        fault_reset_all();
    }
    intf_sys_irq_restore(mstatus);

    return ok ? 0 : -1;
}

app_fault_state_t app_fault_get_state(void) {
    return s_f.state;
}

uint32_t app_fault_get_codes(void) {
    return s_f.active;
}

uint32_t app_fault_get_latched(void) {
    return s_f.latched;
}

uint32_t app_fault_get_first(void) {
    return s_f.first;
}

void app_fault_get_event_counts(uint16_t *out, uint8_t n) {
    if (out == NULL) {
        return;
    }
    for (uint8_t i = 0U; (i < n) && (i < APP_FAULT_EVENT_COUNT); i++) {
        out[i] = s_f.event_cnt[i];
    }
}

bool app_fault_get_snapshot(app_fault_snapshot_t *out) {
    uint32_t mstatus;
    bool ok = false;

    if ((out == NULL) || !s_ready) {
        return false;
    }
    /* 防止与 ISR 侧 fault_raise 的快照写入竞态（撕裂拷贝） */
    mstatus = intf_sys_irq_save();
    if (s_f.snap.code != 0U) {
        *out = s_f.snap;
        ok = true;
    }
    intf_sys_irq_restore(mstatus);
    return ok;
}

void app_fault_get_wdog_raw(uint16_t *thshd_high, uint16_t *thshd_low) {
    if (thshd_high != NULL) {
        *thshd_high = s_f.wdog_thshd_high;
    }
    if (thshd_low != NULL) {
        *thshd_low = s_f.wdog_thshd_low;
    }
}
