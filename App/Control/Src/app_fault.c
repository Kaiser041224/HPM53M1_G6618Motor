/**
 * @file    app_fault.c
 * @brief   故障保护与错误处理（v1：纯判断）
 * @author  Kaiser
 *
 * 三级过流保护 + 链路健康；只记录/输出，不执行任何动作（v2 接入动作层）。
 * 阈值默认值来源：config/software.yaml（经 app_software_params 加载）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_fault.h"
#include "app_ramfunc.h"

#include "algo_rms.h"
#include "app_analog_signal.h"
#include "app_encoder.h"
#include "app_hardware_params.h"
#include "app_software_params.h"
#include "intf_sys.h"

#include <stddef.h>
#include <string.h>

/* Ozone 观测变量（.noncacheable.bss：调试器直读） */
volatile uint32_t g_fault_state __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_fault_codes __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_fault_latched __attribute__((section(".noncacheable.bss")));

#define APP_FAULT_EVENT_COUNT (12U) /* bit0..11 事件计数 */
#define APP_FAULT_OC_COUNT    (3U)

/**
 * @brief 故障保护运行时上下文
 */
typedef struct {
    /* 配置（已解析默认值） */
    float oc_fast_a;          /**< L1 相电流阈值 [A] */
    float oc_slow_a;          /**< L2 RMS 阈值 [A] */
    float vbus_ov_v;          /**< 母线过压阈值 [V] */
    float vbus_uv_v;          /**< 母线欠压阈值 [V] */
    uint16_t slow_debounce;   /**< L2/L3 去抖次数 */
    uint16_t adc_stall_ms;    /**< PMT 帧超时 [ms] */
    uint8_t enc_err_delta;    /**< 编码器错误增量阈值 */
    uint16_t wdog_thshd_high; /**< WDOG 高阈值原始码 */
    uint16_t wdog_thshd_low;  /**< WDOG 低阈值原始码 */

    /* 状态 */
    app_fault_state_t state;                   /**< 当前状态 */
    uint32_t active;                           /**< 当前存在的条件 */
    uint32_t latched;                          /**< 锁存故障 */
    uint32_t first;                            /**< 首故障码 */
    uint16_t event_cnt[APP_FAULT_EVENT_COUNT]; /**< 各故障事件计数 */

    /* 去抖 / 健康 */
    uint16_t oc_slow_cnt[APP_FAULT_OC_COUNT]; /**< L2 各相去抖计数 */
    uint16_t vbus_ov_cnt;                     /**< 过压去抖计数 */
    uint16_t vbus_uv_cnt;                     /**< 欠压去抖计数 */
    uint16_t adc_stall_cnt;                   /**< PMT 帧停滞计数 */
    uint16_t settle_ticks;                    /**< 上电静默期计数 */
    uint32_t enc_err_last;                    /**< 编码器错误计数基线 */
    uint32_t seq_last;                        /**< ADC 帧序号基线 */

    /* L1 WDOG 自动重臂状态（逐相） */
    uint8_t wdog_hit[APP_FAULT_OC_COUNT];     /**< 该相命中待重臂 */
    uint8_t wdog_locked[APP_FAULT_OC_COUNT];  /**< 达上限：不再自动重臂 */
    uint16_t wdog_rearm_cnt[APP_FAULT_OC_COUNT];  /**< 自上次清除以来重臂次数 */
    uint16_t wdog_rearm_wait[APP_FAULT_OC_COUNT]; /**< 剩余等待 tick */
    uint16_t wdog_rearm_ticks;                /**< 等待时间 [ms]（1kHz tick） */
    uint16_t wdog_rearm_max;                  /**< 重臂上限 */

    /* L3 母线两级（2026-10-27：数据源改快通道 + ms 级去抖 + 二级硬跳） */
    float vbus_ov_trip_v;      /**< 二级 OV 阈值 [V]（单拍越限直接 TRIP） */
    uint16_t vbus_ov_ms;       /**< 一级 OV 去抖 [ms]（1kHz tick） */
    uint16_t vbus_uv_ms;       /**< UV 去抖 [ms] */
    uint8_t vbus_ov_trip;      /**< 本拍处于二级越限（L3 状态） */
    uint8_t stop_escalated;    /**< 受控停机不可用 → 动作升 TRIP */
    uint32_t stop_request;     /**< 待发起的受控停机码（边沿） */

    app_fault_snapshot_t snap;                /**< 首故障快照 */
} app_fault_ctx_t;

static app_fault_ctx_t s_fault_ctx;
static bool s_ready;

/* 逐故障动作策略（2026-10-27 v2）。设计表 = 出厂意图；生效表 = 设计表或全 WARN
 * （台架默认 software.fault.actions_enable=0 时全部退为 WARN，行为与 v1 逐字段一致）。 */
static const app_fault_action_t s_policy_design[APP_FAULT_CODE_COUNT] = {
    APP_FLT_ACT_TRIP,   /* 0  OC_FAST_U  —— 硬件级瞬态，不由控制链仲裁 */
    APP_FLT_ACT_TRIP,   /* 1  OC_FAST_V */
    APP_FLT_ACT_TRIP,   /* 2  OC_FAST_W */
    APP_FLT_ACT_DERATE, /* 3  OC_SLOW_U  —— ~250ms 准直流有效值 → 过载语义 */
    APP_FLT_ACT_DERATE, /* 4  OC_SLOW_V */
    APP_FLT_ACT_DERATE, /* 5  OC_SLOW_W */
    APP_FLT_ACT_STOP,   /* 6  VBUS_OV    —— 卸转矩本身即降压，优于断桥 */
    APP_FLT_ACT_STOP,   /* 7  VBUS_UV */
    APP_FLT_ACT_TRIP,   /* 8  ADC_TIMEOUT—— ISR 已死 = 控制链不可信 */
    APP_FLT_ACT_WARN,   /* 9  ENC_READ   —— 事件型，先观察 */
    APP_FLT_ACT_TRIP,   /* 10 OC_TRIP    —— 电流环自身快跳（硬编码动作） */
    APP_FLT_ACT_STOP,   /* 11 COMM_TIMEOUT —— 主机失联：受控停机（不硬断桥） */
};
static app_fault_action_t s_policy[APP_FAULT_CODE_COUNT];
static uint32_t s_action_mask; /**< 生效表中 policy ∈ {STOP,TRIP} 的码位 */
static uint32_t s_injected;    /**< 台架注入集合（独立于 active/latched） */
static float s_derate_scale = APP_FAULT_DERATE_SCALE_DEFAULT;
static bool s_actions_en; /**< 保护动作总开关（源自 software.fault.actions_enable） */

static void fault_rebuild_action_mask(void); /* 前置声明：定义在后 */

/** @brief 由 s_actions_en + 设计表重建生效表与动作掩码 */
static void fault_reload_policy(void) {
    for (uint8_t i = 0U; i < APP_FAULT_CODE_COUNT; i++) {
        s_policy[i] = s_actions_en ? s_policy_design[i] : APP_FLT_ACT_WARN;
    }
    fault_rebuild_action_mask();
}

/** @brief 有限性判定（位模式；与 foc_finite 同语义，避免依赖 Algorithm/FOC 头） */
static bool fault_finite(float x) {
    union {
        float f;
        uint32_t u;
    } v;

    v.f = x;
    return ((v.u & 0x7F800000UL) != 0x7F800000UL);
}

static void fault_publish(void);
static void fault_rebuild_action_mask(void);            /* 前置声明 */

APP_ATTR_RAMFUNC /* 30kHz ISR 路径（WDOG 回调 / FOC 门控） */
static uint32_t fault_code_bit(uint32_t code) {
    for (uint8_t i = 0U; i < APP_FAULT_CODE_COUNT; i++) {
        if ((code & (1UL << i)) != 0U) {
            return (1UL << i);
        }
    }
    return 0U;
}

/** @brief 有效条件位图（物理 active ∪ 注入） */
APP_ATTR_RAMFUNC /* 30kHz ISR 路径（WDOG 回调 / FOC 门控） */
static uint32_t fault_effective_active(void) { return s_fault_ctx.active | s_injected; }
/** @brief 有效锁存位图（物理 latched ∪ 注入） */
APP_ATTR_RAMFUNC
static uint32_t fault_effective_latched(void) { return s_fault_ctx.latched | s_injected; }

/** @brief 重算 state（FAULT 优先）与 Ozone 镜像；tick 与 inject 共用 */
APP_ATTR_RAMFUNC /* 30kHz ISR 路径（WDOG 回调 / FOC 门控） */
static void fault_eval_state(bool pending) {
    uint32_t soft = fault_effective_latched();

    if (app_fault_get_action() == APP_FLT_ACT_TRIP) {
        s_fault_ctx.state = APP_FAULT_STATE_FAULT;
    } else if ((soft != 0U) || pending) {
        s_fault_ctx.state = APP_FAULT_STATE_WARNING;
    } else {
        s_fault_ctx.state = APP_FAULT_STATE_NORMAL;
    }
    fault_publish();
}

/** @brief 由生效表重建"带动作码掩码" */
static void fault_rebuild_action_mask(void) {
    uint32_t mask = 0U;

    for (uint8_t i = 0U; i < APP_FAULT_CODE_COUNT; i++) {
        if ((s_policy[i] == APP_FLT_ACT_STOP) || (s_policy[i] == APP_FLT_ACT_TRIP)) {
            mask |= (1UL << i);
        }
    }
    s_action_mask = mask;
}

/* RMS：三相真有效值（10ms 滑窗 @25kHz） */
static algo_rms_t s_rms[APP_FAULT_OC_COUNT];
static float s_rms_buf[APP_FAULT_OC_COUNT][APP_FAULT_RMS_WINDOW_DEFAULT];

/* -------------------------------------------------------------------------- */

/**
 * @brief 将内部状态镜像到 Ozone 观测变量
 */
APP_ATTR_RAMFUNC /* 30kHz ISR 路径（WDOG 回调 / FOC 门控） */
static void fault_publish(void) {
    g_fault_state = (uint32_t)s_fault_ctx.state;
    g_fault_codes = s_fault_ctx.active;
    g_fault_latched = s_fault_ctx.latched;
}

/**
 * @brief 采集当前物理量快照（模拟量未就绪则跳过）
 */
static void fault_snapshot_now(void) {
    app_analog_values_t v;

    if (app_analog_signal_read_all(&v)) {
        s_fault_ctx.snap.i_u_a = v.i_u_a;
        s_fault_ctx.snap.i_v_a = v.i_v_a;
        s_fault_ctx.snap.i_w_a = v.i_w_a;
        s_fault_ctx.snap.v_bus_v = v.v_bus_v;
    }
}

/**
 * @brief 置位故障：更新有效/锁存/首故障/事件计数/快照/状态。
 *        已锁存的故障只维持 active（不重复计数/不覆盖首故障快照）。
 * @param code 故障码（单个位）
 * @param oc_fast_raw L1 触发时的原始码（0 = 非 L1）
 */
APP_ATTR_RAMFUNC
static void fault_raise(uint32_t code, uint16_t oc_fast_raw) {
    uint8_t bit;

    s_fault_ctx.active |= code;
    fault_publish(); /* 即使已锁存也同步 active 镜像（Ozone 观测变量） */

    if ((s_fault_ctx.latched & code) != 0U) {
        return;      /* 已锁存：仅维持 active */
    }
    s_fault_ctx.latched |= code;

    for (bit = 0U; bit < APP_FAULT_EVENT_COUNT; bit++) {
        if ((code & (1UL << bit)) != 0U) {
            if (s_fault_ctx.event_cnt[bit] < 0xFFFFU) {
                s_fault_ctx.event_cnt[bit]++;
            }
        }
    }

    if (s_fault_ctx.first == 0U) {
        s_fault_ctx.first = code;
        s_fault_ctx.snap.code = code;
        s_fault_ctx.snap.oc_fast_raw = oc_fast_raw;
        fault_snapshot_now();
    }

    /* STOP 不进 FAULT（否则会被 FOC 门控立即急停，违背"受控停机"语义）：
     * 只有 policy == TRIP 的码（或其升级形态）才置 FAULT。 */
    if (app_fault_get_policy(code) == APP_FLT_ACT_TRIP) {
        s_fault_ctx.state = APP_FAULT_STATE_FAULT;
    }
    fault_publish();
}

/**
 * @brief 条件复检：L2/L3 用当前值 + L1 用最新原始值（主循环与 tick 共用）
 * @return true = 故障条件均已恢复
 */
static bool fault_conditions_ok(void) {
    app_analog_values_t v;

    if (app_analog_signal_read_all(&v)) {
        if ((v.v_bus_v > s_fault_ctx.vbus_ov_v) || (v.v_bus_v < s_fault_ctx.vbus_uv_v)) {
            return false;
        }
    }
    for (uint8_t i = 0U; i < APP_FAULT_OC_COUNT; i++) {
        if (s_rms[i].get_rms(&s_rms[i]) > s_fault_ctx.oc_slow_a) {
            return false;
        }
    }
    for (uint8_t ch = (uint8_t)ADC_CH_I_U; ch <= (uint8_t)ADC_CH_I_W; ch++) {
        uint16_t raw = 0U;

        if (!app_adc_get_raw((adc_channel_t)ch, &raw)) {
            return false; /* 无数据视为条件未确认 */
        }
        if ((raw > s_fault_ctx.wdog_thshd_high) || (raw < s_fault_ctx.wdog_thshd_low)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 复位故障状态与去抖计数（清除路径；须在 ISR 上下文调用）。
 *        语义：F = 完整复位——状态/锁存/首故障/快照/事件计数全部归零。
 */
static void fault_reset_all(void) {
    s_fault_ctx.latched = 0U;
    s_fault_ctx.active = 0U;
    s_fault_ctx.first = 0U;
    memset(&s_fault_ctx.snap, 0, sizeof(s_fault_ctx.snap));
    memset(s_fault_ctx.event_cnt, 0, sizeof(s_fault_ctx.event_cnt));
    memset(s_fault_ctx.oc_slow_cnt, 0, sizeof(s_fault_ctx.oc_slow_cnt));
    s_fault_ctx.vbus_ov_cnt = 0U;
    s_fault_ctx.vbus_uv_cnt = 0U;
    s_fault_ctx.adc_stall_cnt = 0U;
    memset(s_fault_ctx.wdog_hit, 0, sizeof(s_fault_ctx.wdog_hit));
    memset(s_fault_ctx.wdog_locked, 0, sizeof(s_fault_ctx.wdog_locked));
    memset(s_fault_ctx.wdog_rearm_cnt, 0, sizeof(s_fault_ctx.wdog_rearm_cnt));
    memset(s_fault_ctx.wdog_rearm_wait, 0, sizeof(s_fault_ctx.wdog_rearm_wait));
    /* 编码器错误计数重新基线（自清除起重新累计） */
    s_fault_ctx.enc_err_last = app_encoder_get_error_count(APP_ENCODER_ROTOR)
                             + app_encoder_get_error_count(APP_ENCODER_OUTPUT);
    s_injected = 0U;
    s_fault_ctx.vbus_ov_trip = 0U;
    s_fault_ctx.stop_escalated = 0U;
    s_fault_ctx.stop_request = 0U;
    s_fault_ctx.state = APP_FAULT_STATE_NORMAL;
    fault_publish();

    /* 重装 L1 WDOG 中断（ISR 命中后驱动自动关闭该通道） */
    for (uint8_t ch = (uint8_t)ADC_CH_I_U; ch <= (uint8_t)ADC_CH_I_W; ch++) {
        app_adc_wdog_reenable((adc_channel_t)ch);
    }
}

/* -------------------------------------------------------------------------- */

void app_fault_init(const app_fault_cfg_t* cfg) {
    algo_rms_cfg_t rms_cfg;
    const app_hardware_params_t* hardware;
    const app_software_params_t* software;
    float dev_v;
    float dev_cnt;
    uint32_t dev;
    uint32_t mid;

    memset(&s_fault_ctx, 0, sizeof(s_fault_ctx));

    /* 运行期参数单例（config/software.yaml + config/hardware.yaml；将来 flash 覆盖） */
    hardware = app_hardware_params_current();
    software = app_software_params_current();

    s_fault_ctx.oc_fast_a =
        ((cfg != NULL) && (cfg->oc_fast_a > 0.0f)) ? cfg->oc_fast_a : software->fault.oc_trip_a;
    s_fault_ctx.oc_slow_a =
        ((cfg != NULL) && (cfg->oc_slow_a > 0.0f)) ? cfg->oc_slow_a : software->fault.oc_trip_a;
    s_fault_ctx.vbus_ov_v =
        ((cfg != NULL) && (cfg->vbus_ov_v > 0.0f)) ? cfg->vbus_ov_v : software->fault.vbus_ov_v;
    s_fault_ctx.vbus_uv_v =
        ((cfg != NULL) && (cfg->vbus_uv_v > 0.0f)) ? cfg->vbus_uv_v : software->fault.vbus_uv_v;
    s_fault_ctx.slow_debounce = ((cfg != NULL) && (cfg->slow_debounce > 0U))
                                  ? cfg->slow_debounce
                                  : software->fault.slow_debounce;
    s_fault_ctx.adc_stall_ms = ((cfg != NULL) && (cfg->adc_stall_ms > 0U))
                                 ? cfg->adc_stall_ms
                                 : software->fault.adc_stall_ms;
    s_fault_ctx.enc_err_delta = ((cfg != NULL) && (cfg->enc_err_delta > 0U))
                                  ? cfg->enc_err_delta
                                  : software->fault.enc_err_delta;

    /* WDOG 原始窗口：中值 ± (阈值[A] / 转换[A/V] → 电压 → 码值)
     * 注1：基于标称零点（半量程）；逐相标定后重装为 v2 精化项
     * 注2：65535 = 16bit 满量程（与 app_adc 默认分辨率一致；改分辨率需同步） */
    dev_v = s_fault_ctx.oc_fast_a / hardware->current_sense.a_per_volt;
    dev_cnt = dev_v * (65535.0f / (INTF_ADC_DEFAULT_VREF_MV / 1000.0f));
    dev = (uint32_t)(dev_cnt + 0.5f);
    mid = 65535U / 2U;
    s_fault_ctx.wdog_thshd_high = (uint16_t)(((mid + dev) > 65535U) ? 65535U : (mid + dev));
    s_fault_ctx.wdog_thshd_low = (uint16_t)((dev > mid) ? 0U : (mid - dev));

    /* RMS 初始化（真有效值，不去直流）：
     * 必须先 ctor（填充 ops 函数指针 + 复位内部状态），再 init —— 否则 ops 为 NULL */
    memset(&rms_cfg, 0, sizeof(rms_cfg));
    rms_cfg.window_size = APP_FAULT_RMS_WINDOW_DEFAULT;
    rms_cfg.remove_dc = false;
    for (uint8_t i = 0U; i < APP_FAULT_OC_COUNT; i++) {
        algo_rms_ctor(&s_rms[i]);
        rms_cfg.buffer = s_rms_buf[i];
        (void)s_rms[i].init(&s_rms[i], &rms_cfg);
    }

    s_actions_en = (software->fault.actions_enable != 0U);
    fault_reload_policy();
    s_injected = 0U;
    /* L3 两级默认：一级沿用 v1 的 5 拍去抖（保持字节级行为一致），二级 40V */
    s_fault_ctx.vbus_ov_trip_v = APP_FAULT_VBUS_OV_TRIP_V_DEFAULT;
    s_fault_ctx.vbus_ov_ms = APP_FAULT_VBUS_DEBOUNCE_MS_DEFAULT;
    s_fault_ctx.vbus_uv_ms = APP_FAULT_VBUS_DEBOUNCE_MS_DEFAULT;
    s_fault_ctx.vbus_ov_trip = 0U;
    s_fault_ctx.stop_escalated = 0U;
    s_fault_ctx.stop_request = 0U;
    s_fault_ctx.wdog_rearm_ticks = APP_FAULT_WDOG_REARM_MS_DEFAULT;
    s_fault_ctx.wdog_rearm_max = APP_FAULT_WDOG_REARM_MAX_DEFAULT;
    s_fault_ctx.enc_err_last = app_encoder_get_error_count(APP_ENCODER_ROTOR)
                             + app_encoder_get_error_count(APP_ENCODER_OUTPUT);
    s_fault_ctx.seq_last = app_adc_get_sequence();
    s_fault_ctx.state = APP_FAULT_STATE_NORMAL;
    s_ready = true;
    fault_publish();
}

APP_ATTR_RAMFUNC
void app_fault_process(void) {
    app_analog_values_t v;

    if (!s_ready || !app_analog_signal_read_all(&v)) {
        return;
    }

    (void)s_rms[0].step(&s_rms[0], v.i_u_a);
    (void)s_rms[1].step(&s_rms[1], v.i_v_a);
    (void)s_rms[2].step(&s_rms[2], v.i_w_a);
}

APP_ATTR_RAMFUNC
void app_fault_tick(void) {
    uint32_t seq;
    float vbus;
    bool pending = false;

    if (!s_ready) {
        return;
    }

    /* 上电静默期：等待 ADC/模拟量/编码器链路稳定（避免启动瞬态误报） */
    if (s_fault_ctx.settle_ticks < APP_FAULT_SETTLE_TICKS_DEFAULT) {
        s_fault_ctx.settle_ticks++;
        s_fault_ctx.seq_last = app_adc_get_sequence();
        return;
    }

    /* 驱动每帧回调一次（SEQ_CMPT） */
    seq = app_adc_get_sequence();

    /* ---- L2：RMS 判断（连续去抖） ---- */
    for (uint8_t i = 0U; i < APP_FAULT_OC_COUNT; i++) {
        float rms = s_rms[i].get_rms(&s_rms[i]);

        if (rms > s_fault_ctx.oc_slow_a) {
            if (s_fault_ctx.oc_slow_cnt[i] < 0xFFFFU) {
                s_fault_ctx.oc_slow_cnt[i]++;
            }
        } else {
            s_fault_ctx.oc_slow_cnt[i] = 0U;
        }

        if (s_fault_ctx.oc_slow_cnt[i] >= s_fault_ctx.slow_debounce) {
            fault_raise(APP_FAULT_OC_SLOW_U << i, 0U);
        } else if (s_fault_ctx.oc_slow_cnt[i] > 0U) {
            pending = true;
        }
    }

    /* ---- L3：母线判断（**快通道值** + ms 级去抖；二级无去抖直跳） ----
     * 数据源改用与三相电流同源同拍的 30kHz 快通道（转换延迟从 1ms 降到 1 拍）；
     * 快通道不可用（尚无首帧/NAN）时回退 1kHz 缓存，保留启动期语义。
     * 物理依据：母线过压的危害是电容能量/持续抬压（ms 级），µs 级尖峰由缓冲网络
     * 与 TVS 负责 —— 因此本级有意义的时间尺度是 ms，不是拍。 */
    vbus = app_analog_signal_vbus_fast();
    if (!fault_finite(vbus)) {
        vbus = app_analog_signal_read(ADC_CH_V_VBUS);
    }
    /* 二级：单拍越限直接置状态；仅当该码已被授予动作时 get_action 才会升级为 TRIP */
    s_fault_ctx.vbus_ov_trip =
        (vbus > s_fault_ctx.vbus_ov_trip_v && app_fault_get_policy(APP_FAULT_VBUS_OV)
                                                  >= APP_FLT_ACT_STOP)
            ? 1U
            : 0U;
    if (vbus > s_fault_ctx.vbus_ov_v) {
        if (s_fault_ctx.vbus_ov_cnt < 0xFFFFU) {
            s_fault_ctx.vbus_ov_cnt++;
        }
    } else {
        s_fault_ctx.vbus_ov_cnt = 0U;
    }
    if (vbus < s_fault_ctx.vbus_uv_v) {
        if (s_fault_ctx.vbus_uv_cnt < 0xFFFFU) {
            s_fault_ctx.vbus_uv_cnt++;
        }
    } else {
        s_fault_ctx.vbus_uv_cnt = 0U;
    }
    if ((s_fault_ctx.vbus_ov_cnt >= s_fault_ctx.vbus_ov_ms) || (s_fault_ctx.vbus_ov_trip != 0U)) {
        fault_raise(APP_FAULT_VBUS_OV, 0U);
        /* STOP 类动作不由 state 触发（state 只在 TRIP 时置 FAULT）→ 发一次边沿请求 */
        if ((app_fault_get_action() == APP_FLT_ACT_STOP) && (s_fault_ctx.stop_request == 0U)) {
            s_fault_ctx.stop_request = APP_FAULT_VBUS_OV;
        }
    } else if (s_fault_ctx.vbus_ov_cnt > 0U) {
        pending = true;
    }
    if (s_fault_ctx.vbus_uv_cnt >= s_fault_ctx.vbus_uv_ms) {
        fault_raise(APP_FAULT_VBUS_UV, 0U);
        if ((app_fault_get_action() == APP_FLT_ACT_STOP) && (s_fault_ctx.stop_request == 0U)) {
            s_fault_ctx.stop_request = APP_FAULT_VBUS_UV;
        }
    } else if (s_fault_ctx.vbus_uv_cnt > 0U) {
        pending = true;
    }

    /* ---- 健康：PMT 帧停滞 ---- */
    if (seq == s_fault_ctx.seq_last) {
        if (s_fault_ctx.adc_stall_cnt < 0xFFFFU) {
            s_fault_ctx.adc_stall_cnt++;
        }
    } else {
        s_fault_ctx.seq_last = seq;
        s_fault_ctx.adc_stall_cnt = 0U;
    }
    if (s_fault_ctx.adc_stall_cnt
        > s_fault_ctx.adc_stall_ms) { /* >10ms：第 11 个无新帧 tick 触发 */
        fault_raise(APP_FAULT_ADC_TIMEOUT, 0U);
    }

    /* ---- 健康：编码器错误增量 ---- */
    {
        uint32_t ec = app_encoder_get_error_count(APP_ENCODER_ROTOR)
                    + app_encoder_get_error_count(APP_ENCODER_OUTPUT);

        if ((ec - s_fault_ctx.enc_err_last) >= (uint32_t)s_fault_ctx.enc_err_delta) {
            s_fault_ctx.enc_err_last = ec;
            fault_raise(APP_FAULT_ENC_READ, 0U);
        }
    }

    /* ---- 条件恢复时清除 active（latched 保持锁存；ENC_READ 为事件型仅经清除解除） ---- */
    for (uint8_t i = 0U; i < APP_FAULT_OC_COUNT; i++) {
        if (s_fault_ctx.oc_slow_cnt[i] == 0U) {
            s_fault_ctx.active &= ~(APP_FAULT_OC_SLOW_U << i);
        }
    }
    if (s_fault_ctx.vbus_ov_cnt == 0U) {
        s_fault_ctx.active &= ~APP_FAULT_VBUS_OV;
    }
    if (s_fault_ctx.vbus_uv_cnt == 0U) {
        s_fault_ctx.active &= ~APP_FAULT_VBUS_UV;
    }
    if (s_fault_ctx.adc_stall_cnt == 0U) {
        s_fault_ctx.active &= ~APP_FAULT_ADC_TIMEOUT;
    }
    /* 停机升级位随母线条件一并清除：否则空闲期一次越限会让此后所有 STOP 永久变 TRIP */
    if ((s_fault_ctx.vbus_ov_cnt == 0U) && (s_fault_ctx.vbus_uv_cnt == 0U)
        && (s_fault_ctx.vbus_ov_trip == 0U)) {
        s_fault_ctx.stop_escalated = 0U;
    }
    for (uint8_t ch = (uint8_t)ADC_CH_I_U; ch <= (uint8_t)ADC_CH_I_W; ch++) {
        uint16_t raw = 0U;

        if (app_adc_get_raw((adc_channel_t)ch, &raw) && (raw <= s_fault_ctx.wdog_thshd_high)
            && (raw >= s_fault_ctx.wdog_thshd_low)) {
            s_fault_ctx.active &= ~(APP_FAULT_OC_FAST_U << (uint8_t)(ch - (uint8_t)ADC_CH_I_U));
        }
    }

    /* ---- 状态汇总 ----
     * state 由 app_fault_eval_state() 统一计算：**仅当生效动作为 TRIP 才进 FAULT**。
     * STOP 不进 FAULT（否则会被 FOC 门控立即急停，违背"受控停机"本意）→ 由
     * app_fault_take_stop_request() 以边沿请求交 app_foc_run_once 发起软停机。
     * 台架默认 software.fault.actions_enable=0 时生效表全部退为 WARN → 与 v1 逐字段一致。
     * 总开关 = software.fault.actions_enable（运行期可 fault protect on/off 覆盖）。 */
    fault_eval_state(pending);
}

APP_ATTR_RAMFUNC
void app_fault_on_wdog(adc_channel_t ch, uint16_t value, void* user) {
    (void)user;

    if (!s_ready || (ch < ADC_CH_I_U) || (ch > ADC_CH_I_W)) {
        return;
    }
    /* 上电静默期：电流运放输出尚未建立（实测：此刻 PMT 结果可能整帧为 0/全 1，
     * 快照 vbus=0、三相电流=0）→ 硬件 WDOG 会在启动瞬态假触发并锁存三路 OC_FAST，
     * 后果是 actions 打开时**每次开机直接进 FAULT、拒绝 foc on**。
     * 与软件判据共用 settle_ticks（由 app_fault_tick 推进）；此时桥尚未使能，
     * 忽略不会失去任何真实保护。 */
    if (s_fault_ctx.settle_ticks < APP_FAULT_SETTLE_TICKS_DEFAULT) {
        return;
    }
    {
        uint8_t i = (uint8_t)(ch - ADC_CH_I_U);

        /* 驱动在本回调返回后会自动关闭该通道 WDOG 中断 → 标记待重臂 */
        s_fault_ctx.wdog_hit[i] = 1U;
        s_fault_ctx.wdog_rearm_wait[i] = s_fault_ctx.wdog_rearm_ticks;
        fault_raise(APP_FAULT_OC_FAST_U << i, value);
    }
}

/**
 * @brief L1 WDOG 自动重臂服务（1kHz 任务上下文；契约见 app_fault.h）
 */
APP_ATTR_RAMFUNC
void app_fault_wdog_service(void) {
    if (!s_ready) {
        return;
    }
    for (uint8_t i = 0U; i < APP_FAULT_OC_COUNT; i++) {
        uint16_t raw = 0U;
        adc_channel_t ch = (adc_channel_t)((uint8_t)ADC_CH_I_U + i);

        if ((s_fault_ctx.wdog_hit[i] == 0U) || (s_fault_ctx.wdog_locked[i] != 0U)) {
            continue;
        }
        /* 等待期：命中后静默 rearm_ticks 拍，避免瞬态期间立刻重臂又立刻命中 */
        if (s_fault_ctx.wdog_rearm_wait[i] > 0U) {
            s_fault_ctx.wdog_rearm_wait[i]--;
            continue;
        }
        /* 电流必须已回到窗内才重臂；持续超窗 = 真实过流，不重臂 */
        if (!app_adc_get_raw(ch, &raw) || (raw > s_fault_ctx.wdog_thshd_high)
            || (raw < s_fault_ctx.wdog_thshd_low)) {
            continue;
        }
        if (s_fault_ctx.wdog_rearm_cnt[i] >= s_fault_ctx.wdog_rearm_max) {
            /* 达上限：升 TRIP 锁存，不再自动重臂（仅 fault clear 复位） */
            s_fault_ctx.wdog_locked[i] = 1U;
            s_fault_ctx.wdog_hit[i] = 0U;
            fault_raise(APP_FAULT_OC_FAST_U << i, 0U);
            continue;
        }
        app_adc_wdog_reenable(ch);
        if (s_fault_ctx.wdog_rearm_cnt[i] < 0xFFFFU) {
            s_fault_ctx.wdog_rearm_cnt[i]++;
        }
        s_fault_ctx.wdog_hit[i] = 0U;
    }
}

void app_fault_set_wdog_rearm(uint16_t rearm_ms, uint16_t rearm_max) {
    if (rearm_ms > 0U) {
        s_fault_ctx.wdog_rearm_ticks = rearm_ms;
    }
    if (rearm_max > 0U) {
        s_fault_ctx.wdog_rearm_max = rearm_max;
    }
}

uint16_t app_fault_get_wdog_rearm_count(uint8_t phase) {
    if (phase >= APP_FAULT_OC_COUNT) {
        return 0U;
    }
    return s_fault_ctx.wdog_rearm_cnt[phase];
}

APP_ATTR_RAMFUNC
int app_fault_clear(void) {
    uint32_t mstatus;
    bool ok;

    if (!s_ready) {
        return -1;
    }

    /* 临界区：与 ADC0/ADC1 故障 ISR 互斥。本构建 ISR **不可抢占**
     * （CMakeLists: DISABLE_IRQ_PREEMPTIVE=1 → port.c:336 的 freertos_handle_interrupt
     *  不重开 MIE），故 ISR 一旦进入即运行至完成；但"复检 → 复位"之间仍可能被
     *  WDOG 的 ISR 插入（视为独立事件），因此仍需关中断保护。 */
    mstatus = intf_sys_irq_save();
    ok = fault_conditions_ok();
    if (ok) {
        fault_reset_all();
    }
    intf_sys_irq_restore(mstatus);

    return ok ? 0 : -1;
}

APP_ATTR_RAMFUNC /* FOC ISR 门控 */
app_fault_state_t app_fault_get_state(void) { return s_fault_ctx.state; }

uint32_t app_fault_get_codes(void) { return fault_effective_active(); }

uint32_t app_fault_get_latched(void) { return fault_effective_latched(); }

uint32_t app_fault_get_injected(void) { return s_injected; }

/* ---- policy / 分级动作 / 注入（2026-10-27 v2）---- */

app_fault_action_t app_fault_policy_default(uint32_t code) {
    uint32_t bit = fault_code_bit(code);

    return (bit == 0U) ? APP_FLT_ACT_WARN : s_policy_design[__builtin_ctz(bit)];
}

APP_ATTR_RAMFUNC
app_fault_action_t app_fault_get_policy(uint32_t code) {
    uint32_t bit = fault_code_bit(code);

    return (bit == 0U) ? APP_FLT_ACT_WARN : s_policy[__builtin_ctz(bit)];
}

void app_fault_set_policy(uint32_t code, app_fault_action_t act) {
    uint32_t bit = fault_code_bit(code);

    if (bit == 0U) {
        return;
    }
    s_policy[__builtin_ctz(bit)] = act;
    fault_rebuild_action_mask();
    fault_eval_state(false);
}

APP_ATTR_RAMFUNC
app_fault_action_t app_fault_get_action(void) {
    uint32_t soft = fault_effective_latched();
    uint32_t live = fault_effective_active();
    app_fault_action_t act = APP_FLT_ACT_WARN;

    for (uint8_t i = 0U; i < APP_FAULT_CODE_COUNT; i++) {
        app_fault_action_t p;
        bool in_latched = ((soft & (1UL << i)) != 0U);
        bool in_active = ((live & (1UL << i)) != 0U);

        if (!in_latched && !in_active) {
            continue;
        }
        p = s_policy[i];
        /* DERATE 是"条件恢复自动解除" → 依赖 active；STOP/TRIP 依赖锁存（需 fault clear） */
        if ((p == APP_FLT_ACT_DERATE) ? !in_active : !in_latched) {
            continue;
        }
        /* 二级母线越限：仅当该码已被授予动作（STOP/TRIP）时升级为 TRIP */
        if (((1UL << i) == APP_FAULT_VBUS_OV) && (s_fault_ctx.vbus_ov_trip != 0U)
            && (p >= APP_FLT_ACT_STOP)) {
            p = APP_FLT_ACT_TRIP;
        }
        /* 受控停机不可用（调用方报告）→ 同样升为 TRIP */
        if ((s_fault_ctx.stop_escalated != 0U) && (p == APP_FLT_ACT_STOP)) {
            p = APP_FLT_ACT_TRIP;
        }
        if (p > act) {
            act = p;
        }
    }
    return act;
}

APP_ATTR_RAMFUNC
float app_fault_get_limit_scale(void) {
    /* 依 **active**（非 latched）：过载条件消失即应回到 1.0，否则一次 OC_SLOW 会把
     * iq 上限永久压到 0.5× 直到人工 fault clear。 */
    uint32_t live = fault_effective_active();

    for (uint8_t i = 0U; i < APP_FAULT_CODE_COUNT; i++) {
        if (((live & (1UL << i)) != 0U) && (s_policy[i] == APP_FLT_ACT_DERATE)) {
            return s_derate_scale;
        }
    }
    return 1.0f;
}

uint32_t app_fault_get_blocking_latched(void) { return fault_effective_latched() & s_action_mask; }

APP_ATTR_RAMFUNC /* 30kHz ISR 门控调用 */
bool app_fault_actions_enabled(void) { return s_actions_en; }

void app_fault_set_actions_enable(bool on) {
    s_actions_en = on;
    fault_reload_policy();
    fault_eval_state(false);
}

void app_fault_set_derate_scale(float scale) {
    if (fault_finite(scale) && (scale > 0.0f) && (scale <= 1.0f)) {
        s_derate_scale = scale;
    }
}

void app_fault_set_vbus_levels(float ov_v, float ov_trip_v, float uv_v, uint16_t ov_ms,
                               uint16_t uv_ms) {
    if (fault_finite(ov_v) && (ov_v > 0.0f)) {
        s_fault_ctx.vbus_ov_v = ov_v;
    }
    if (fault_finite(ov_trip_v) && (ov_trip_v > 0.0f)) {
        s_fault_ctx.vbus_ov_trip_v = ov_trip_v;
    }
    if (fault_finite(uv_v) && (uv_v > 0.0f)) {
        s_fault_ctx.vbus_uv_v = uv_v;
    }
    if (ov_ms > 0U) {
        s_fault_ctx.vbus_ov_ms = ov_ms;
    }
    if (uv_ms > 0U) {
        s_fault_ctx.vbus_uv_ms = uv_ms;
    }
}

uint32_t app_fault_take_stop_request(void) {
    uint32_t code = s_fault_ctx.stop_request;

    s_fault_ctx.stop_request = 0U;
    return code;
}

void app_fault_note_stop_rejected(void) {
    s_fault_ctx.stop_escalated = 1U;
    fault_eval_state(false);
}

int app_fault_inject(uint32_t code, bool on) {
    uint32_t bit = fault_code_bit(code);

    if (bit == 0U) {
        return -1;
    }
    if (on) {
        s_injected |= bit;
        /* 注入也留首故障：否则 fault show 出现 latched≠0 而 first=0 的矛盾（台架实测） */
        if (s_fault_ctx.first == 0U) {
            s_fault_ctx.first = bit;
            s_fault_ctx.snap.code = bit;
            fault_snapshot_now();
        }
    } else {
        s_injected &= ~bit;
        /* 撤销注入时同步撤销该码的待处理停机请求：否则 1ms 内 on→off 会残留一次误停机 */
        if (s_fault_ctx.stop_request == bit) {
            s_fault_ctx.stop_request = 0U;
        }
    }
    /* STOP 类注入也必须产生一次受控停机请求：否则台架只能看到 action=stop/state=WARNING，
     * 桥仍带电、无任何实际动作 → STOP 路径无法验证（实测缺口）。边沿触发，与 L3 同源。 */
    if (on && (app_fault_get_action() == APP_FLT_ACT_STOP) && (s_fault_ctx.stop_request == 0U)) {
        s_fault_ctx.stop_request = bit;
    }
    fault_eval_state(false);
    return 0;
}

APP_ATTR_RAMFUNC
void app_fault_raise(uint32_t code) {
    if (code != 0U) {
        fault_raise(fault_code_bit(code), 0U);
    }
}

uint32_t app_fault_get_first(void) { return s_fault_ctx.first; }

void app_fault_get_event_counts(uint16_t* out, uint8_t n) {
    if (out == NULL) {
        return;
    }
    for (uint8_t i = 0U; (i < n) && (i < APP_FAULT_EVENT_COUNT); i++) {
        out[i] = s_fault_ctx.event_cnt[i];
    }
}

bool app_fault_get_snapshot(app_fault_snapshot_t* out) {
    uint32_t mstatus;
    bool ok = false;

    if ((out == NULL) || !s_ready) {
        return false;
    }
    /* 防止与 ISR 侧 fault_raise 的快照写入竞态（撕裂拷贝） */
    mstatus = intf_sys_irq_save();
    if (s_fault_ctx.snap.code != 0U) {
        *out = s_fault_ctx.snap;
        ok = true;
    }
    intf_sys_irq_restore(mstatus);
    return ok;
}

void app_fault_get_wdog_raw(uint16_t* thshd_high, uint16_t* thshd_low) {
    if (thshd_high != NULL) {
        *thshd_high = s_fault_ctx.wdog_thshd_high;
    }
    if (thshd_low != NULL) {
        *thshd_low = s_fault_ctx.wdog_thshd_low;
    }
}
