# 故障保护与错误处理实施计划（v1：纯判断）

> **实施后修订记录（2026-09-20）**：本计划为实现前的设计稿；落地后经两轮评审修订，
> 以下与本文档不同，以代码与设计文档为准：
> 1. 清除改为**临界区直执行**（`intf_sys_irq_save/restore` 保护复检+完整复位，非请求式）；
> 2. 新增**上电静默期**（50 tick 内跳过 L2/L3/健康判断）；
> 3. 移除 tick 的"按帧去重"（曾致 ADC 停滞检测死代码）——驱动改为每帧仅回调一次；
> 4. `algo_rms` 必须先 `algo_rms_ctor()` 再 `init()`；
> 5. 条件恢复自动解除 `active`；`F` 为完整复位（含事件计数/编码器基线）。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 App 层新增 `App/Control/app_fault`——三级过流保护（L1 ADC WDOG 硬件快判 / L2 RMS 慢判 / L3 母线）+ 链路健康检测（ADC 帧超时 / 编码器读失败），输出故障码与状态机；**只判断不动作**。

**Architecture:** 新增 `App/Control/` 层（AGENTS.md §2.2 保护策略归属）。`app_fault` 只依赖 Interface 与 Platform；接线由 `App/Logic` 编排：L1 = ADC0 WDOG 中断回调；L2/L3/健康 = ADC1 序列完成回调（1kHz，ISR，帧去重）；L2 RMS 在 25kHz 主循环累加。

**Tech Stack:** C17、HPM SDK（ADC16 WDOG / TRGM / GPTMR）、`algo_rms`、现有 `app_adc`/`app_analog_signal`/`app_encoder`。

**规范依据:** `docs/superpowers/specs/2026-09-19-fault-protection-design.md`（已评审确认）

**验证说明（嵌入式适配）:** 本工程无主机侧单测框架；每个任务的验证 = 编译通过 + 台架步骤（用户执行）。构建命令：`make build`（在项目根目录，产物 `output/HPM53M1_G6618Motor.elf`）。

---

## 文件结构

**新建：**
- `App/Control/Inc/app_fault.h` — 故障码/状态/配置/API
- `App/Control/Src/app_fault.c` — 检测 + 状态机 + 记录
- `App/Debug/Inc/app_debug_fault.h` — `f`/`F` 命令声明
- `App/Debug/Src/app_debug_fault.c` — 命令实现

**修改：**
- `CMakeLists.txt` — Control 层 include + 源码 + debug 源码
- `Driver/hpm_impl/drv_adc.c` — PMT 分支支持 WDOG；WDOG 值源改 `PRD_RESULT`
- `App/Platform/Inc/app_adc.h` / `Src/app_adc.c` — cfg 扩展（WDOG + slow_cb）+ 回调适配 + `app_adc_wdog_reenable()`
- `App/Platform/Inc/app_analog_signal.h` — 暴露电流转换常数 `APP_ANALOG_I_AMP_PER_VOLT`
- `App/Logic/app_logic.c` — 接线（init 顺序 + 25kHz 调用）
- `App/Debug/Src/app_debug_cmd.c` — `f`/`F` 分发

---

### Task 1: CMake 与 Control 层骨架

**Files:**
- Create: `App/Control/Inc/app_fault.h`
- Create: `App/Control/Src/app_fault.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 创建目录与最小骨架**

`App/Control/Inc/app_fault.h`：

```c
/*
 * App Fault - 故障保护与错误处理（v1：纯判断）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_FAULT_H
#define APP_FAULT_H

#include "app_adc.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 故障码（32 位位图；bit10-31 预留：温度等）
 * ============================================================================ */
#define APP_FAULT_NONE        (0x00000000UL)
#define APP_FAULT_OC_FAST_U   (1UL << 0)  /* L1：ADC WDOG 硬件阈值（µs 级） */
#define APP_FAULT_OC_FAST_V   (1UL << 1)
#define APP_FAULT_OC_FAST_W   (1UL << 2)
#define APP_FAULT_OC_SLOW_U   (1UL << 3)  /* L2：RMS 连续越限（1kHz 去抖） */
#define APP_FAULT_OC_SLOW_V   (1UL << 4)
#define APP_FAULT_OC_SLOW_W   (1UL << 5)
#define APP_FAULT_VBUS_OV     (1UL << 6)  /* L3：母线过压 */
#define APP_FAULT_VBUS_UV     (1UL << 7)  /* L3：母线欠压 */
#define APP_FAULT_ADC_TIMEOUT (1UL << 8)  /* 健康：PMT 帧序号停滞 */
#define APP_FAULT_ENC_READ    (1UL << 9)  /* 健康：编码器错误计数增量 */

/* ============================================================================
 * 状态机
 * ============================================================================ */
typedef enum {
    APP_FAULT_STATE_INIT = 0,
    APP_FAULT_STATE_NORMAL,
    APP_FAULT_STATE_WARNING, /* 去抖计数中（条件已出现，未达次数） */
    APP_FAULT_STATE_FAULT,   /* 已触发（锁存；v1 无动作） */
} app_fault_state_t;

/* ============================================================================
 * 默认阈值（2026-09-19 评审确认）
 *   L1/L2 相电流 = 3 × 电机手册峰值 24.3A（G66-18）= 72.9A
 *   L3 母线：OV 36V / UV 9V（24V 系统）
 * ============================================================================ */
#define APP_FAULT_OC_TRIP_A_DEFAULT     (72.9f)
#define APP_FAULT_VBUS_OV_V_DEFAULT     (36.0f)
#define APP_FAULT_VBUS_UV_V_DEFAULT     (9.0f)
#define APP_FAULT_SLOW_DEBOUNCE_DEFAULT (5U)   /* L2/L3 连续次数（1kHz） */
#define APP_FAULT_ADC_STALL_MS_DEFAULT  (10U)  /* PMT 帧停滞 [ms] */
#define APP_FAULT_ENC_ERR_DELTA_DEFAULT (3U)   /* 编码器错误增量 */
#define APP_FAULT_RMS_WINDOW_DEFAULT    (250U) /* 10ms @25kHz */

typedef struct {
    float    oc_fast_a;     /* L1 相电流阈值 [A]，0 = 默认 72.9A */
    float    oc_slow_a;     /* L2 RMS 阈值 [A]，0 = 默认 72.9A */
    float    vbus_ov_v;     /* 过压 [V]，0 = 默认 36V */
    float    vbus_uv_v;     /* 欠压 [V]，0 = 默认 9V */
    uint16_t slow_debounce; /* L2/L3 去抖次数，0 = 默认 5 */
    uint16_t adc_stall_ms;  /* PMT 帧超时 [ms]，0 = 默认 10 */
    uint8_t  enc_err_delta; /* 编码器错误增量阈值，0 = 默认 3 */
} app_fault_cfg_t;

/* 首故障快照（进入 FAULT 时记录一次） */
typedef struct {
    uint32_t code;        /* 首故障码（单个位） */
    float    i_u_a;
    float    i_v_a;
    float    i_w_a;
    float    v_bus_v;
    uint16_t oc_fast_raw; /* L1 触发时的原始码（0 = 非 L1） */
} app_fault_snapshot_t;

/* ============================================================================
 * API
 * ============================================================================ */

/** @brief 初始化（须在 app_adc_init 之前调用：提供 WDOG 阈值与回调） */
void app_fault_init(const app_fault_cfg_t *cfg); /* NULL = 全默认 */

/** @brief 25kHz 主循环：三相电流 RMS 累加 */
void app_fault_process(void);

/** @brief 1kHz（ADC1 序列完成回调，ISR 上下文）：L2/L3/健康判断（按帧去重） */
void app_fault_tick(void);

/** @brief L1：ADC0 WDOG 回调（ISR 上下文；ch = 逻辑通道） */
void app_fault_on_wdog(adc_channel_t ch, uint16_t value, void *user);

/** @brief 清除锁存（条件须已恢复）；0 成功，-1 拒绝 */
int app_fault_clear(void);

app_fault_state_t app_fault_get_state(void);
uint32_t app_fault_get_codes(void);    /* 当前有效条件位图 */
uint32_t app_fault_get_latched(void);  /* 锁存故障位图 */
uint32_t app_fault_get_first(void);    /* 首故障码 */
void app_fault_get_event_counts(uint16_t *out, uint8_t n); /* 各故障事件计数（按 bit0..） */
bool app_fault_get_snapshot(app_fault_snapshot_t *out);

/** @brief 取 WDOG 原始阈值窗口（供 app_adc 配置；init 后有效） */
void app_fault_get_wdog_raw(uint16_t *thshd_high, uint16_t *thshd_low);

#ifdef __cplusplus
}
#endif

#endif /* APP_FAULT_H */
```

`App/Control/Src/app_fault.c`（Task 5 填充实现；本步先建最小可编译骨架）：

```c
/*
 * App Fault - 故障保护与错误处理（v1：纯判断）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_fault.h"

#include <stddef.h>
#include <string.h>

static bool s_ready;

void app_fault_init(const app_fault_cfg_t *cfg) {
    (void) cfg;
    s_ready = true;
}
```

- [ ] **Step 2: CMake 接入**

`CMakeLists.txt`：
1. 在 `# Include Paths` 区（`App/Platform/Inc` 行后）加：
```cmake
sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/App/Control/Inc)
```
2. 在 `# Platform Layer` 段后新增段：
```cmake
# ============================================================================
# Control Layer
# ============================================================================

sdk_app_src(App/Control/Src/app_fault.c)
```
3. 在 `# Debug Layer` 段加：
```cmake
sdk_app_src(App/Debug/Src/app_debug_fault.c)
```
（`app_debug_fault.c` 在 Task 6 创建；若先加会构建失败 → 本步只加前两项，Task 6 再加第三项。）

- [ ] **Step 3: 构建验证**

Run: `make build 2>&1 | tail -5`
Expected: `BUILD SUCCESS`（无 warning/error）

---

### Task 2: drv_adc 扩展（PMT WDOG + 值源）

**Files:**
- Modify: `Driver/hpm_impl/drv_adc.c`

- [ ] **Step 1: PMT 分支支持 WDOG 配置**

`adc_init()` 的 PMT 分支中，逐通道 `adc16_init_channel` 调用处（现为 `ch_cfg.ch = cfg->pmt_ch_list[i]; if (adc16_init_channel(...)`），在循环前补充：

```c
        adc16_channel_config_t ch_cfg;
        adc16_get_channel_default_config(&ch_cfg);
        ch_cfg.sample_cycle = sample_cycle;
        if (cfg->wdog_en) {
            ch_cfg.wdog_int_en = true;
            ch_cfg.thshdh = cfg->wdog_thshd_high;
            ch_cfg.thshdl = cfg->wdog_thshd_low;
        }
```

循环内 `adc16_init_channel` 成功后补充（与 oneshot 分支一致）：

```c
            if (cfg->wdog_en) {
                ai->wdog.enabled[cfg->pmt_ch_list[i]] = true;
                ai->wdog.cb = cfg->wdog_cb;
                ai->wdog.cb_user_data = cfg->wdog_cb_user_data;
                adc16_enable_interrupts(ai->base, (uint32_t) (1u << cfg->pmt_ch_list[i]));
            }
```

并把 PMT 分支尾部的中断使能条件改为（WDOG 也需要 IRQ）：

```c
        if ((cfg->pmt_cb != NULL) || cfg->wdog_en) {
            adc_enable_instance_irq(inst);
        }
```

- [ ] **Step 2: WDOG 值源改 `PRD_RESULT`（全模式通用）**

ISR 的 wdog 分支（现读 `base->BUS_RESULT[ch]` + VALID 判断）改为：

```c
            if ((wdog_status & (1u << ch)) && ai->wdog.enabled[ch]) {
                /* PRD_RESULTx 保存通道 x 最近一次转换结果（所有模式通用，PMT 已实测） */
                uint16_t val = (uint16_t) ADC16_PRD_CFG_PRD_RESULT_CHAN_RESULT_GET(
                    base->PRD_CFG[ch].PRD_RESULT);
                if (ai->wdog.cb) {
                    ai->wdog.cb(INTF_ADC_CH(inst, ch), val, ai->wdog.cb_user_data);
                }
                adc16_disable_interrupts(base, (uint32_t) (1u << ch));
            }
```

- [ ] **Step 3: 构建验证**

Run: `make build 2>&1 | tail -5`
Expected: `BUILD SUCCESS`

---

### Task 3: app_adc 扩展（WDOG 透传 + 回调适配 + 默认值守卫）

**Files:**
- Modify: `App/Platform/Inc/app_adc.h`
- Modify: `App/Platform/Src/app_adc.c`

- [ ] **Step 1: `app_adc_cfg_t` 扩展（`app_adc.h`）**

```c
/* WDOG 回调（逻辑通道，非硬件通道） */
typedef void (*app_adc_wdog_cb_t)(adc_channel_t ch, uint16_t value, void *user);

typedef struct {
    uint32_t trigger_delay_ns; /* 谷底后触发延时 [ns]（0 = 默认 500ns） */
    uint16_t sample_cycle;     /* 采样窗口 [ADC 时钟数]（0 = 默认） */
    uint8_t  resolution;       /* intf_adc_resolution_t（0 = 默认 16bit） */
    /* ADC0 电流通道 WDOG（硬件阈值；0 = 关闭） */
    bool     wdog_en;
    uint16_t wdog_thshd_high;
    uint16_t wdog_thshd_low;
    app_adc_wdog_cb_t wdog_cb;
    void    *wdog_cb_user;
    /* ADC1 序列完成回调（1kHz，ISR 上下文；NULL = 不启用） */
    void   (*slow_cb)(void);
} app_adc_cfg_t;
```

并声明：

```c
/** @brief 重装指定通道的 WDOG 中断（故障清除后调用） */
void app_adc_wdog_reenable(adc_channel_t ch);
```

- [ ] **Step 2: `app_adc.c` 适配与守卫**

（a）默认值守卫（`app_adc_init` 内，与 sample_cycle/resolution 并列）：

```c
        if (s_cfg.trigger_delay_ns == 0U) {
            s_cfg.trigger_delay_ns = APP_ADC_TRIGGER_DELAY_NS_DEFAULT;
        }
```

（b）文件顶部加内部适配器（`s_map` 之后）：

```c
/* 驱动 WDOG 回调（硬件通道）→ 逻辑通道回调适配 */
static void adc_wdog_adapter(intf_adc_ch_t ch, uint16_t value, void *user) {
    uint8_t inst = INTF_ADC_CH_INST(ch);
    uint8_t hw = INTF_ADC_CH_IDX(ch);

    (void) user;
    for (uint8_t i = 0U; i < (uint8_t) ADC_CH_COUNT; i++) {
        if ((s_map[i].inst == inst) && (s_map[i].hw_ch == hw)) {
            if (s_cfg.wdog_cb != NULL) {
                s_cfg.wdog_cb((adc_channel_t) i, value, s_cfg.wdog_cb_user);
            }
            return;
        }
    }
}

/* 驱动 SEQ 完成回调 → 用户慢帧回调适配（1kHz，ISR） */
static void adc_slow_adapter(intf_adc_ch_t ch, void *user) {
    (void) ch;
    (void) user;
    if (s_cfg.slow_cb != NULL) {
        s_cfg.slow_cb();
    }
}
```

（c）ADC0 配置补 WDOG 字段（`a0` 组包处）：

```c
    a0.wdog_en = s_cfg.wdog_en;
    a0.wdog_thshd_high = s_cfg.wdog_thshd_high;
    a0.wdog_thshd_low = s_cfg.wdog_thshd_low;
    a0.wdog_cb = adc_wdog_adapter;
    a0.wdog_cb_user_data = NULL;
```

（d）ADC1 配置补慢帧回调：

```c
    a1.seq_cb = (s_cfg.slow_cb != NULL) ? adc_slow_adapter : NULL;
```

（e）新增公开 API（文件末尾）：

```c
void app_adc_wdog_reenable(adc_channel_t ch) {
    if (ch >= ADC_CH_COUNT) {
        return;
    }
    intf_adc_wdog_reenable(INTF_ADC_CH(s_map[ch].inst, s_map[ch].hw_ch));
}
```

- [ ] **Step 3: 构建验证**

Run: `make build 2>&1 | tail -5`
Expected: `BUILD SUCCESS`（注意：此时 app_fault 仍是骨架，无行为变化）

---

### Task 4: app_analog_signal 暴露电流转换常数

**Files:**
- Modify: `App/Platform/Inc/app_analog_signal.h`

- [ ] **Step 1: 头文件加常数（换算公式注释下方）**

```c
/* 电流链路转换常数 [A/V]（实际实现名：APP_ANALOG_I_AMP_PER_VOLT，见 app_analog_signal.h） */
#define APP_ANALOG_I_AMP_PER_VOLT (66.6667f)
```

并在 `app_analog_signal.c` 中将原字面量 `66.6667f` 替换为该宏（保证单一来源）。

- [ ] **Step 2: 构建验证**

Run: `make build 2>&1 | tail -5`
Expected: `BUILD SUCCESS`

---

### Task 5: app_fault 模块（核心）

**Files:**
- Modify: `App/Control/Src/app_fault.c`（替换骨架为完整实现）

- [ ] **Step 1: 完整实现**

```c
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

#include <stddef.h>
#include <string.h>

/* Ozone 观测变量（.noncacheable.bss：调试器直读） */
volatile uint32_t g_fault_state __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_fault_codes __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_fault_latched __attribute__((section(".noncacheable.bss")));

#define APP_FAULT_EVENT_COUNT   (10U)  /* bit0..9 事件计数 */
#define APP_FAULT_OC_COUNT      (3U)

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
    uint32_t enc_err_last;
    uint32_t seq_last;
    uint32_t tick_seq; /* 已处理的帧序号（回调去重） */

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

/* 置位故障：更新有效/锁存/首故障/事件计数/快照/状态 */
static void fault_raise(uint32_t code, uint16_t oc_fast_raw) {
    uint8_t bit;

    s_f.active |= code;
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

/* -------------------------------------------------------------------------- */

void app_fault_init(const app_fault_cfg_t *cfg) {
    algo_rms_cfg_t rms_cfg;
    float dev_v;
    float dev_cnt;
    uint32_t dev;
    uint32_t mid;

    memset(&s_f, 0, sizeof(s_f));

    s_f.oc_fast_a = ((cfg != NULL) && (cfg->oc_fast_a > 0.0f))
                        ? cfg->oc_fast_a : APP_FAULT_OC_TRIP_A_DEFAULT;
    s_f.oc_slow_a = ((cfg != NULL) && (cfg->oc_slow_a > 0.0f))
                        ? cfg->oc_slow_a : APP_FAULT_OC_TRIP_A_DEFAULT;
    s_f.vbus_ov_v = ((cfg != NULL) && (cfg->vbus_ov_v > 0.0f))
                        ? cfg->vbus_ov_v : APP_FAULT_VBUS_OV_V_DEFAULT;
    s_f.vbus_uv_v = ((cfg != NULL) && (cfg->vbus_uv_v > 0.0f))
                        ? cfg->vbus_uv_v : APP_FAULT_VBUS_UV_V_DEFAULT;
    s_f.slow_debounce = ((cfg != NULL) && (cfg->slow_debounce > 0U))
                        ? cfg->slow_debounce : APP_FAULT_SLOW_DEBOUNCE_DEFAULT;
    s_f.adc_stall_ms = ((cfg != NULL) && (cfg->adc_stall_ms > 0U))
                        ? cfg->adc_stall_ms : APP_FAULT_ADC_STALL_MS_DEFAULT;
    s_f.enc_err_delta = ((cfg != NULL) && (cfg->enc_err_delta > 0U))
                        ? cfg->enc_err_delta : APP_FAULT_ENC_ERR_DELTA_DEFAULT;

    /* WDOG 原始窗口：中值 ± (阈值[A] / 转换[A/V] → 电压 → 码值)
     * 注：基于标称零点（半量程）；逐相标定后重装为 v2 精化项 */
    dev_v = s_f.oc_fast_a / APP_ANALOG_I_AMP_PER_VOLT;
    dev_cnt = dev_v * (65535.0f / (INTF_ADC_DEFAULT_VREF_MV / 1000.0f));
    dev = (uint32_t) (dev_cnt + 0.5f);
    mid = 65535U / 2U;
    s_f.wdog_thshd_high = (uint16_t) (((mid + dev) > 65535U) ? 65535U : (mid + dev));
    s_f.wdog_thshd_low = (uint16_t) ((dev > mid) ? 0U : (mid - dev));

    /* RMS 初始化（真有效值，不去直流） */
    memset(&rms_cfg, 0, sizeof(rms_cfg));
    rms_cfg.window_size = APP_FAULT_RMS_WINDOW_DEFAULT;
    rms_cfg.remove_dc = false;
    for (uint8_t i = 0U; i < APP_FAULT_OC_COUNT; i++) {
        rms_cfg.buffer = s_rms_buf[i];
        (void) s_rms[i].init(&s_rms[i], &rms_cfg);
    }

    s_f.enc_err_last = app_encoder_get_error_count(APP_ENCODER_ROTOR)
                     + app_encoder_get_error_count(APP_ENCODER_OUTPUT);
    s_f.seq_last = app_adc_get_sequence();
    s_f.tick_seq = s_f.seq_last;
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

    /* 驱动对 SEQ_CVC/CMPT 各回调一次 → 按帧去重 */
    seq = app_adc_get_sequence();
    if (seq == s_f.tick_seq) {
        return;
    }
    s_f.tick_seq = seq;

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
    if (s_f.adc_stall_cnt > s_f.adc_stall_ms) {
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
    bool cond_ok = true;

    if (!s_ready) {
        return -1;
    }

    /* 条件须已恢复：L2/L3 用当前值复检；L1 用最新原始值复检 WDOG 窗口 */
    {
        app_analog_values_t v;

        if (app_analog_signal_read_all(&v)) {
            if ((v.v_bus_v > s_f.vbus_ov_v) || (v.v_bus_v < s_f.vbus_uv_v)) {
                cond_ok = false;
            }
        }
        for (uint8_t i = 0U; i < APP_FAULT_OC_COUNT; i++) {
            if (s_rms[i].get_rms(&s_rms[i]) > s_f.oc_slow_a) {
                cond_ok = false;
            }
        }
    }
    for (uint8_t ch = (uint8_t) ADC_CH_I_U; ch <= (uint8_t) ADC_CH_I_W; ch++) {
        uint16_t raw = 0U;

        if (app_adc_get_raw((adc_channel_t) ch, &raw)) {
            if ((raw > s_f.wdog_thshd_high) || (raw < s_f.wdog_thshd_low)) {
                cond_ok = false;
            }
        }
    }
    if (!cond_ok) {
        return -1;
    }

    s_f.latched = 0U;
    s_f.active = 0U;
    s_f.first = 0U;
    s_f.state = APP_FAULT_STATE_NORMAL;

    /* 重装 L1 WDOG 中断（ISR 命中后驱动自动关闭该通道） */
    for (uint8_t ch = (uint8_t) ADC_CH_I_U; ch <= (uint8_t) ADC_CH_I_W; ch++) {
        app_adc_wdog_reenable((adc_channel_t) ch);
    }

    fault_publish();
    return 0;
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
    if ((out == NULL) || (s_f.snap.code == 0U)) {
        return false;
    }
    *out = s_f.snap;
    return true;
}

void app_fault_get_wdog_raw(uint16_t *thshd_high, uint16_t *thshd_low) {
    if (thshd_high != NULL) {
        *thshd_high = s_f.wdog_thshd_high;
    }
    if (thshd_low != NULL) {
        *thshd_low = s_f.wdog_thshd_low;
    }
}
```

- [ ] **Step 2: 构建验证**

Run: `make build 2>&1 | tail -5`
Expected: `BUILD SUCCESS`（无 warning；注意 `app_fault_clear` 中 `s_f.first = 0U` 后 `get_snapshot` 会返回 false，符合"清除后无首故障"语义）

---

### Task 6: 调试层（f/F 命令）

**Files:**
- Create: `App/Debug/Inc/app_debug_fault.h`
- Create: `App/Debug/Src/app_debug_fault.c`
- Modify: `App/Debug/Src/app_debug_cmd.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 调试模块**

`App/Debug/Inc/app_debug_fault.h`：

```c
/*
 * Debug Fault - 故障保护调试（f：打印 / F：清除锁存）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_DEBUG_FAULT_H
#define APP_DEBUG_FAULT_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 命令 f：打印故障状态机 / 故障码 / 计数 / 首故障快照 */
void app_debug_fault_dump(void);

/** @brief 命令 F：清除锁存（条件须已恢复） */
void app_debug_fault_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_DEBUG_FAULT_H */
```

`App/Debug/Src/app_debug_fault.c`：

```c
/*
 * Debug Fault - 故障保护调试实现
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_debug_fault.h"

#include "app_debug_rtt.h"
#include "app_fault.h"

#include <stddef.h>

static const char *const s_state_names[] = {
    "INIT", "NORMAL", "WARNING", "FAULT",
};

void app_debug_fault_dump(void) {
    uint16_t cnt[10] = {0};
    app_fault_snapshot_t snap;
    uint32_t state = (uint32_t) app_fault_get_state();

    app_debug_printf("[FLT] state=%s codes=0x%08X latched=0x%08X first=0x%08X\r\n",
                     (state < 4U) ? s_state_names[state] : "?",
                     (unsigned) app_fault_get_codes(), (unsigned) app_fault_get_latched(),
                     (unsigned) app_fault_get_first());

    app_fault_get_event_counts(cnt, 10U);
    app_debug_printf("[FLT] cnt: F_U=%u F_V=%u F_W=%u S_U=%u S_V=%u S_W=%u OV=%u UV=%u TO=%u ENC=%u\r\n",
                     (unsigned) cnt[0], (unsigned) cnt[1], (unsigned) cnt[2],
                     (unsigned) cnt[3], (unsigned) cnt[4], (unsigned) cnt[5],
                     (unsigned) cnt[6], (unsigned) cnt[7], (unsigned) cnt[8], (unsigned) cnt[9]);

    if (app_fault_get_snapshot(&snap)) {
        app_debug_printf("[FLT] trip: code=0x%08X iu=%.3fA iv=%.3fA iw=%.3fA vbus=%.2fV raw=%u\r\n",
                         (unsigned) snap.code, (double) snap.i_u_a, (double) snap.i_v_a,
                         (double) snap.i_w_a, (double) snap.v_bus_v, (unsigned) snap.oc_fast_raw);
    } else {
        app_debug_printf("[FLT] trip: (none)\r\n");
    }
}

void app_debug_fault_clear(void) {
    if (app_fault_clear() == 0) {
        app_debug_printf("[FLT] clear OK\r\n");
    } else {
        app_debug_printf("[FLT] clear REJECTED (condition active)\r\n");
    }
}
```

- [ ] **Step 2: 命令分发（`app_debug_cmd.c`）**

顶部加 `#include "app_debug_fault.h"`；在 switch 中（`case 'p':` 附近）加：

```c
        case 'f':
            app_debug_fault_dump();
            break;
        case 'F':
            app_debug_fault_clear();
            break;
```

- [ ] **Step 3: CMake 加 debug 源**

```cmake
sdk_app_src(App/Debug/Src/app_debug_fault.c)
```

- [ ] **Step 4: 构建验证**

Run: `make build 2>&1 | tail -5`
Expected: `BUILD SUCCESS`

---

### Task 7: app_logic 接线

**Files:**
- Modify: `App/Logic/app_logic.c`

- [ ] **Step 1: 初始化顺序（步骤 9 之前插入故障保护；ADC 初始化改为带配置）**

```c
    /* 9. 故障保护（在 ADC 之前：提供 WDOG 阈值与回调） */
    app_fault_init(NULL);

    /* 10. ADC 采样链（ADC0 电流 + WDOG；ADC1 慢通道序列 + 1kHz 故障 tick） */
    {
        app_adc_cfg_t adc_cfg;
        uint16_t wdog_hi;
        uint16_t wdog_lo;

        memset(&adc_cfg, 0, sizeof(adc_cfg));
        app_fault_get_wdog_raw(&wdog_hi, &wdog_lo);
        adc_cfg.wdog_en = true;
        adc_cfg.wdog_thshd_high = wdog_hi;
        adc_cfg.wdog_thshd_low = wdog_lo;
        adc_cfg.wdog_cb = app_fault_on_wdog;
        adc_cfg.wdog_cb_user = NULL;
        adc_cfg.slow_cb = app_fault_tick;
        app_adc_init(&adc_cfg);
    }
    app_analog_signal_init();
    app_debug_adc_init();
```

（原 `app_adc_init(NULL);` 行删除；后续步骤编号顺延。）

- [ ] **Step 2: 25kHz 主循环调用（`app_run` 内，`app_analog_signal_process()` 之后）**

```c
            app_analog_signal_process();
            app_fault_process(); /* 25kHz：三相电流 RMS 累加 */
```

- [ ] **Step 3: 头文件包含**

`app_logic.c` 顶部加：

```c
#include "app_fault.h"
```

（`memset` 需要 `<string.h>`，检查是否已包含。）

- [ ] **Step 4: 构建验证**

Run: `make build 2>&1 | tail -5`
Expected: `BUILD SUCCESS`

---

### Task 8: 构建与台架验证（用户执行）

**Files:** 无（验证步骤）

- [ ] **Step 1: 全量构建 + 导出**

Run: `make build 2>&1 | tail -5 && make artifacts 2>&1 | tail -8`
Expected: `BUILD SUCCESS` + artifacts 刷新（`output/HPM53M1_G6618Motor.elf`）

- [ ] **Step 2: 上电基线（外部电源）**

烧录后确认：
1. 启动横幅与自检正常（与保护前一致）；
2. `f` → `state=NORMAL codes=0x00000000`（无故障）；
3. `d`/`p` 行为与保护前一致（回归）；
4. 旋转（`r`）→ 电流/慢通道正常，`f` 仍 NORMAL（3% 调制远低于 72.9A）。

- [ ] **Step 3: L1 WDOG 台架自测（关键验证点）**

临时把 `app_fault_init` 的 WDOG 下限抬高（或临时将 `oc_fast_a` 传小值，如 `app_fault_cfg_t{.oc_fast_a=1.0f}`），使静态电流值越界：
- 预期：`f` → `state=FAULT`、`codes=0x1/0x2/0x4`（对应相）、`cnt` 增加、`trip` 快照有值；
- **确认 WDOG 在 PMT 模式下工作**；验证后恢复默认阈值（重建）。
- 若 WDOG 不工作（无触发）：记录结论，回退方案 = 25kHz PMT 回调内软件峰值比较（v2 项，需另行设计）。

- [ ] **Step 4: L2/L3 注入验证**

临时小阈值（如 `{.oc_slow_a=1.0f, .vbus_ov_v=20.0f}`）：
- 预期：`f` 观察到 `WARNING` → 连续 5 次后 `FAULT`（S_U/S_V/S_W、OV 位）；
- 验证后恢复默认。

- [ ] **Step 5: 清除与健康项**

- `F`（条件恢复后）→ `clear OK`，状态回 NORMAL；条件未恢复时 → `clear REJECTED`；
- 编码器错误注入（拔一路 SPI）→ `ENC` 计数增加；
- 全程回归：桥臂/PWM/旋转不受任何影响（纯判断）。

- [ ] **Step 6: 文档与提交（待 Kaiser 确认）**

- 更新 `docs/superpowers/specs/2026-09-19-fault-protection-design.md` 的验证记录节；
- 提交信息建议：`feat(fault): App/Control 故障保护与错误处理（v1 纯判断）`；
- **提交时机由 Kaiser 决定。**

---

## 自检记录（Self-Review）

- **Spec 覆盖**：故障码表→Task 5；状态机→Task 5；L1 WDOG→Task 2/3/5；L2/L3/健康→Task 5；接口→Task 3/5；阈值→Task 5；输出→Task 6；接线→Task 7；验证→Task 8。✓
- **占位符扫描**：无 TBD/TODO；所有步骤含完整代码或明确命令。✓
- **类型一致性**：`app_adc_wdog_cb_t`（Task 3）↔ `app_fault_on_wdog`（Task 5）签名一致；`app_fault_get_wdog_raw` ↔ Task 7 调用一致；`app_debug_fault_*` ↔ Task 6 分发一致。✓
- **已知偏差**：无主机侧单测（嵌入式工程无测试框架）；验证 = 编译 + 台架步骤。
