# 编码器独立采样器 + FOC 实时域解耦 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** 把转子编码器 SPI 采样从 25kHz ADC ISR 迁到 12.5kHz GPTMR ISR（高优先级、单一所有者、一致快照），修复坏帧发布缺陷与电流环竞态/限幅，补齐 Ozone 调试与宿主回归测试。

**Architecture:** 纯算法 `algo_encoder_snapshot` 承载快照/seqlock/坏帧策略；`app_encoder` 成为 SPI3 单一所有者并暴露快照；GPTMR1_CH3@12.5kHz 驱动采样；ADC0@25kHz FOC 只读快照、用样本时间戳算 dt；电流控制器修正 PI 圆限幅并去 bang-bang；`app_debug_foc` 提供 Ozone 结构。

**Tech Stack:** C17、HPM5361 SDK、GPTMR/PLIC、seqlock、宿主 `cc` 测试。

**依据:** `docs/superpowers/specs/2026-09-22-encoder-sampler-foc-realtime-design.md`（用户已批准）。

**约束:** 不改 `config/*.yaml`（保留用户脏值 kp=0.3/ki=100/i_trip=0/speed=0）；不提交 git。

---

## 文件结构

- Create: `App/Algorithm/Inc/algo_encoder_snapshot.h`、`App/Algorithm/Src/algo_encoder_snapshot.c`
- Create: `App/Debug/Inc/app_debug_foc.h`、`App/Debug/Src/app_debug_foc.c`
- Create: `scripts/tests/encoder/{run.sh,test_main.c,test_algo_encoder_snapshot.c,test_util.h,mock_*.c}`
- Modify: `App/Platform/Inc/app_encoder.h`、`App/Platform/Src/app_encoder.c`
- Modify: `App/Platform/Inc/app_gptmr.h`、`App/Platform/Src/app_gptmr.c`
- Modify: `App/Control/Src/app_foc.c`、`App/Control/Src/app_foc_current.c`、`App/Control/Inc/app_foc_current.h`
- Modify: `App/Algorithm/FOC/Inc/foc_modulation.h`、`Src/foc_modulation.c`（新增 `foc_modulation_vmax`）
- Modify: `Driver/hpm_impl/drv_spi.c`（cycle 期限）
- Modify: `App/Debug/Inc/app_debug_encoder.h`、`Src/app_debug_encoder.c`（passive/energized）
- Modify: `App/Logic/app_logic.c`（采样器启动、调试模式）
- Modify: `CMakeLists.txt`（新源文件）

---

## Task 1: 纯算法 `algo_encoder_snapshot`（TDD）

**Files:** Create `App/Algorithm/Inc/algo_encoder_snapshot.h`, `App/Algorithm/Src/algo_encoder_snapshot.c`,
`scripts/tests/encoder/*`.

- [ ] **Step 1: 写失败测试**（覆盖：坏帧不污染发布值；连续失败→invalid；seq 只在接受时推进；
  age 时效；seqlock 有界读返回一致快照；复用检测）
- [ ] **Step 2: 运行测试确认失败**（`scripts/tests/encoder/run.sh`）
- [ ] **Step 3: 最小实现**（结构体 + push + 有界 read + age + is_new）
- [ ] **Step 4: 运行测试确认通过**
- [ ] **Step 5: 不提交**（用户要求无 commit）

关键 API：
```c
typedef struct {
    uint16_t raw;               /* 已接受的原始值（坏帧不写入） */
    uint16_t source_raw;        /* 最近一次设备读到的原始值（含被拒） */
    uint32_t seq;               /* 接受样本计数 */
    uint32_t timestamp_cycles;  /* 最近接受样本时刻 */
    bool     valid;             /* 快照有效 */
    bool     jumped;            /* 最近样本因跳变被拒 */
    bool     read_failed;       /* 最近读失败 */
    uint16_t consecutive_fail;  /* 连续失败/跳变计数 */
    uint32_t age_cycles;        /* 读取时计算的年龄 */
} algo_encoder_sample_t;

typedef struct { /* 对象：匿名结构体接口 */
    ...
    int  (*push)(algo_encoder_snapshot_t*, const algo_encoder_input_t*);
    bool (*read)(algo_encoder_snapshot_t*, algo_encoder_sample_t*, uint8_t max_retries);
    bool (*is_new)(const algo_encoder_sample_t*, uint32_t last_seq);
} algo_encoder_snapshot_t;
```

---

## Task 2: `app_encoder` 集成（单一所有者 + 快照）

**Files:** Modify `App/Platform/Inc/app_encoder.h`, `Src/app_encoder.c`.

- [ ] 新增 `app_encoder_rotor_snapshot_t` 与 `app_encoder_get_rotor_snapshot()`。
- [ ] `app_encoder_sample_rotor_at(uint32_t now_cycles)`：SPI 读 → `algo push`（唯一物理读点）。
- [ ] `app_encoder_sampler_claim(void)`：声明运行期 SPI3 由采样器拥有。
- [ ] `app_encoder_read_raw(ROTOR)`：采样器已 claim 时返回快照（**不碰 SPI**）。
- [ ] `app_encoder_get_rotor_raw/rad` 改从快照取，保留兼容签名。
- [ ] `app_encoder_get_rotor_jump_count()` 取 algo 计数。

## Task 3: GPTMR1_CH3 @12.5kHz 采样器

**Files:** Modify `App/Platform/Inc/app_gptmr.h`, `Src/app_gptmr.c`, `Src/app_encoder.c`.

- [ ] `APP_GPTMR_CH_3`（GPTMR1 CH3，全局 7）；`APP_GPTMR_CH3_FREQ=12500`。
- [ ] `app_encoder_sampler_start()`：`app_gptmr_register_callback(CH_3, isr)` + `app_gptmr_start(CH_3)`。
- [ ] 文档化 PLIC：GPTMR=3 > ADC0=2（不修改驱动默认，除非需显式断言）。
- [ ] 断言不占用 GPTMR0 CH2（全局 2）。

## Task 4: FOC 消费快照（复用 + 时间戳 dt + 统一计时）

**Files:** Modify `App/Control/Src/app_foc.c`.

- [ ] 删除 ISR 内 `app_encoder_sample_rotor()`。
- [ ] 用 `app_encoder_get_rotor_snapshot()`：`seq` 未变→复用上拍 `θe/ωe`；变化→`dt=Δt_ts` step。
- [ ] 时效/连续性判据：`valid`+`age<=stale`+`consecutive_fail<limit`。
- [ ] ISR 单一 exit 统一计算 `g_foc_isr_cycles`、预算检查（所有路径含故障）。
- [ ] 故障门控仅 `FAULT`（非 WARNING）；跳闸 ISR 直接 `app_3phase_inverter_emergency_stop()`。

## Task 5: 电流控制器修正

**Files:** Modify `App/Control/Src/app_foc_current.c`, `Inc/app_foc_current.h`,
`App/Algorithm/FOC/{Inc/foc_modulation.h,Src/foc_modulation.c}`, `Src/app_foc.c`.

- [ ] 新增纯函数 `float foc_modulation_vmax(float duty_max, float v_bus_v)` = `(2D-1)·vbus/√3`；宿主测试。
- [ ] `app_foc_current_run_fresh` 用 `foc_modulation_vmax`（修 `/1.5`）。
- [ ] 移除 vsat bang-bang 置零；保留 `saturated` 标志与抗饱和。
- [ ] `disable` 顺序：zero → disable → reset → OFF。
- [ ] vtest 改为请求 + ISR 执行（单一输出所有者），用真实 dt。

## Task 6: SPI 时间界

**Files:** Modify `Driver/hpm_impl/drv_spi.c`.

- [ ] `spi_frame_fast` 三处等待增加 `intf_clock_get_cycle()` 期限（如 20µs），超时返回 -1；
      保留循环上限并注释语义（修复“循环数≠时间”）。

## Task 7: `app_debug_foc`（Ozone）

**Files:** Create `App/Debug/Inc/app_debug_foc.h`, `Src/app_debug_foc.c`; Modify `CMakeLists.txt`,
`App/Logic/app_logic.c`, `App/Platform/Src/app_encoder.c`(可选上报)。

- [ ] `volatile app_debug_foc_t g_app_debug_foc` in `.noncacheable.bss`。
- [ ] 请求域 + 状态域；一次性命令 ack/result；`app_debug_foc_tick()` 主循环有界处理。
- [ ] 无 Control→Debug 依赖；紧急停机走已有关断。

## Task 8: 编码器自检（passive vs energized）

**Files:** Modify `App/Debug/Inc/app_debug_encoder.h`, `Src/app_debug_encoder.c`.

- [ ] `g_enc_passive_ok/app_enc_passive_*` 与 `g_enc_energized_ok/..._not_run` 字段分离。

## Task 9: 仅调试台架模式

**Files:** Modify `App/Logic/app_logic.c`, `App/Debug/Inc/app_debug_foc.h`.

- [ ] 编译期 `APP_BENCH_DEBUG_MODE`：跳过 USB/终端的 init/run；默认 OFF 零给定。

## Task 10: 宿主测试（mocks）

**Files:** `scripts/tests/encoder/*`（mock SPI/encoder/param/clock；app_encoder.c 编译）。

- [ ] 坏帧不发布、采样器所有权（运行期读不触发 SPI transfer）、陈旧/复用 seq、模式/调制限幅。
- [ ] `scripts/tests/encoder/run.sh` + 既有 `scripts/tests/foc/run.sh` 均通过。

## Task 11: 构建与文档

- [ ] `make build OPT_LEVEL_DBG=-Og`（Debug）通过，记录产物。
- [ ] `make CMAKE_BUILD_TYPE=Release OPT_LEVEL_REL=-O3 build` 通过。
- [ ] 更新 README/Doc 摘要：设计表、Ozone 变量与工作流、未决硬件约束。

---

## 验证清单

- [ ] `scripts/tests/encoder/run.sh` 0 失败
- [ ] `scripts/tests/foc/run.sh` 0 失败
- [ ] Debug `-Og` 构建 exit 0
- [ ] Release 构建 exit 0
- [ ] git status 仍只显示 `config/software.yaml` 用户改动（无 commit）
