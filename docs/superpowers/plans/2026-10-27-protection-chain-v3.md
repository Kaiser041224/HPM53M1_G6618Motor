# 保护链 v3 实施计划（ILM → ADC1 去中断 → WDOG 前移 → policy → L3 → EWDG → DERATE）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement task-by-task.
> Steps use checkbox (`- [ ]`) syntax.

**Goal:** 把 v1 的"只判断"保护接到动作层，并把慢保护链从 XIP/ADC1 中断迁到 ILM + 任务，使 FOC 快环（实测 50% CPU）零扰动、保护获得独立时基。

**Architecture:** 判据按物理时间尺度分层。L1（三相电流硬件 WDOG + `i_trip_a`）留在 30kHz ADC0 PMT ISR；L2/L3/L4/健康 由 1kHz io 任务承载；EWDG 兜 CPU/链死。ADC1 去周期中断，转换保持 30kHz 同源。

**Tech Stack:** C17、HPM5361 SDK、自研 Interface/Driver 分层、宿主机自测套件（`scripts/tests/*`，两档 `-O1` / `-O2 -ffast-math`）。

**Spec:** `docs/superpowers/specs/2026-10-27-protection-action-layer-design.md`（§4 以本计划的最终设计为准）+ `docs/superpowers/specs/2026-09-19-fault-protection-design.md`

## Global Constraints

- `CMAKE_C_STANDARD 17`；热路径函数必须 `APP_ATTR_RAMFUNC`（ILM `0x0000xxxx`）。
- 宿主机测试 pass 2 用 `-O2 -ffast-math` → **测试内禁用 `isnan()/isfinite()`**（会被常量折叠），用位模式判定（同 `foc_finite`）。
- 禁止改动：ADC0 PMT 队列形态（≤4 槽、槽0=队尾）、ADC1 SEQ 槽数（5，`_Static_assert` 已守）、`pwm_freq_hz`、`DISABLE_IRQ_PREEMPTIVE=1`、`USE_NONVECTOR_MODE=1`。
- 每任务结束跑全量：`adc analog control foc encoder identify params` + `scripts/test_gen_params.py` + `make artifacts`（0 warning）+ `git diff --check`。
- 保护动作默认全 WARN（`APP_PROTECT_ACTION_EN=0` 亦保留为台架总保险）→ 未显式放开的阶段行为必须与今天逐字段一致。
- 已知事实（不得回退）：`app_fault.c:424` 注释"SDK ISR 使能嵌套"**与构建事实相反**（非抢占），修正时保留依据。

## Review Focus

- 保护链在 ILM 之外取指（功能正常但延迟尖峰）——`nm` 守卫覆盖。
- vbus 快通道新鲜度握手在被改后恒判陈旧 → 静默回退 1kHz（`g_foc_vbus_src_used` 变 0）。
- `app_fault_tick` 迁到任务后在 `foc off` 的 3s 阻塞 / flash job 窗口内不推进 → 保护有洞。
- STOP 在 OFF-but-bridge-enabled / CALIB / vtest 下无效（应降级 TRIP）。
- public 契约 API 漏挂在 `intf_default.c` 弱实现上 → 链接到空实现。

---

### Task 1: 布局守卫 + 慢保护链 ILM 迁移

**Files:**
- Create: `scripts/tests/layout/run.sh`、`scripts/tests/layout/check_layout.sh`
- Modify: `App/Control/Src/app_fault.c`（`app_fault_tick/process/on_wdog/clear` 加 `APP_ATTR_RAMFUNC`）
- Modify: `App/Platform/Src/app_analog_signal.c`（`app_analog_signal_read_all` 同上）
- Modify: `App/Algorithm/Src/algo_rms.c`（`*_impl` 与内部静态函数同上）
- Test: `scripts/tests/layout/check_layout.sh`

**Interfaces:**
- Produces: 一个可执行的布局守卫脚本：`scripts/tests/layout/check_layout.sh <elf>` → 0 = 全部符号在 ILM，非 0 = 列出落在 XIP 的符号。
- Consumes: `output/HPM53M1_G6618Motor.elf`（需先 `make artifacts`）。

- [ ] **Step 1: 写守卫脚本（RED）**

`scripts/tests/layout/check_layout.sh`：用 `$GNURISCV_TOOLCHAIN_PATH/bin/riscv32-unknown-elf-nm` 取符号地址，对清单逐项要求 `0x0000xxxx`；任何一项落在 `0x8xxxxxxx` 即非 0 退出并打印。

符号清单（Task 1 范围）：`app_fault_tick app_fault_process app_fault_on_wdog app_fault_clear app_analog_signal_read_all algo_rms_step_impl algo_rms_get_rms_impl algo_rms_update_sq_impl algo_rms_calc_mean_sq algo_rms_rebuild_sum_sq`

- [ ] **Step 2: 运行守卫，确认失败**

Run: `make artifacts >/dev/null && bash scripts/tests/layout/check_layout.sh output/HPM53M1_G6618Motor.elf`
Expected: 非 0 退出，列出 10 个符号全部在 `0x8001xxxx`。

- [ ] **Step 3: 加 ILM 属性**

`app_fault.c`：`app_fault_tick`、`app_fault_process`、`app_fault_on_wdog`、`app_fault_clear` 各加 `APP_ATTR_RAMFUNC`。
`app_analog_signal.c`：`app_analog_signal_read_all` 加 `APP_ATTR_RAMFUNC`。
`algo_rms.c`：`algo_rms_step_impl`/`get_rms_impl`/`update_sq_impl`/`calc_mean_sq`/`rebuild_sum_sq` 加 ILM 属性宏（`algo_rms.h` 若无则新定义 `ALGO_RMS_ATTR_RAMFUNC`，语义同 `APP_ATTR_RAMFUNC`）。
理由：`algo_rms` 走函数指针，无法内联；`app_analog_signal_read_all` 在 `app_fault_process` 热路径上。

同时**修正** `app_fault.c:424` 的注释为"本构建 ISR 不可抢占（`DISABLE_IRQ_PREEMPTIVE=1`，`port.c:336` 不重开 MIE）"。

- [ ] **Step 4: 运行守卫，确认通过**

Run: `make artifacts >/dev/null && bash scripts/tests/layout/check_layout.sh output/HPM53M1_G6618Motor.elf`
Expected: 退出 0，打印 10/10 在 ILM。

- [ ] **Step 5: 全量回归 + 提交**

Run: 全量套件 + gen + build（见 Global Constraints）
Expected: 全 0 failures；GEN 23/23；0 warning。
```bash
git add -A && git commit -m "perf(fault): 慢保护链迁入 ILM（fault/analog/rms）+ 布局守卫测试"
```

---

### Task 2: ADC1 去周期中断 + 慢 tick 迁 io 任务 + vbus 新鲜度改 ADC0 帧计数

**Files:**
- Modify: `App/Logic/app_logic.c`（`adc_cfg.slow_cb = NULL`；`app_fault_tick()+app_fault_process()` 移入 `app_io_step` 顶部）
- Modify: `App/Platform/Src/app_adc.c`（vbus 新鲜度改用 `app_adc_get_sequence()`；删分频器 `s_slow_cb_*`/`adc_slow_adapter`）
- Modify: `Driver/hpm_impl/drv_adc.c`（新增 SEQ 推进计数读：`ADC16_Type.SEQ_WR_ADDR`）
- Modify: `Interface/intf_adc.h` + `Interface/intf_default.c`（`intf_adc_get_seq_progress(inst,&wr)`）
- Test: `scripts/tests/adc/test_app_adc_seq_progress.c`

**Interfaces:**
- Produces: `int intf_adc_get_seq_progress(uint8_t inst, uint32_t* wr);`（0 = 成功；读 `SEQ_WR_ADDR`，无中断时"SEQ 在推进"的唯一硬件证据）
- Produces: `app_fault_tick(void)` 语义改为**任务上下文 1kHz**调用（非 ISR）。
- Consumes: Task 1 的 ILM 布局。

- [ ] **Step 1: 写失败测试**

`test_app_adc_seq_progress.c`：mock 一个 `intf_adc_get_seq_progress` 返回递增 wr；断言"wr 不推进 = 判定 SEQ 停摆"。同时断言 `app_fault_tick` 不再被 ADC1 回调驱动（`g_mock_adc_cfg[1].seq_cb == NULL`）。

- [ ] **Step 2: 运行，确认失败**

Run: `bash scripts/tests/adc/run.sh`
Expected: FAIL（`seq_cb != NULL`、`intf_adc_get_seq_progress` 未定义）。

- [ ] **Step 3: 实现**

`app_logic.c`：`adc_cfg.slow_cb` 置 `NULL`（`app_adc.c:325` 会自动使 `seq_cb = NULL` → ADC1 无中断）；`app_io_step` 顶部顺序改为
`app_debug_encoder_sample(); app_adc_slow_process(); app_analog_signal_process(); app_fault_process(); app_fault_tick(); app_foc_run_once(); ...`
`app_adc.c`：vbus 快通道新鲜度把 `intf_adc_get_seq_frames(s_vbus_fast_inst,&f)` 换成 `app_adc_get_sequence()`（ADC0 PMT 帧数，与 ADC1 同源触发）；删除 `s_slow_cb_divisor/s_slow_cb_counter/adc_slow_adapter`。
`drv_adc.c`：新增 `adc_get_seq_progress()` 读 `base->SEQ_WR_ADDR`；注册到 `intf_adc`。
**两处阻塞窗口泵**：`app_foc_wait_off()` 的自旋循环内、`app_terminal_job` 的 flash 保存路径内，各调用一次 `app_fault_tick()`（任务上下文，MIE 可关；链已 ILM）。

- [ ] **Step 4: 运行，确认通过**

Run: `bash scripts/tests/adc/run.sh && bash scripts/tests/analog/run.sh && bash scripts/tests/control/run.sh`
Expected: 全 PASS。

- [ ] **Step 5: 全量回归 + 提交**

Expected: `adc diag` 不再有 ADC1 ISR 统计；`fault` 逐字段与改前一致。
```bash
git add -A && git commit -m "refactor(fault): ADC1 去周期中断，慢 tick 迁 1kHz 任务；vbus 新鲜度改 ADC0 帧计数"
```

---

### Task 3: WDOG 状态评估前移 + 自动重臂

**Files:**
- Modify: `Driver/hpm_impl/drv_adc.c`（`adc_generic_isr` 中 WDOG 分支移到 `TRIG_CMPT`/`SEQ_CMPT` 分支**之前**）
- Modify: `App/Control/Src/app_fault.c`（`app_fault_wdog_service()`：`wdog_rearm_ms` 到期 ∧ 三相 raw 回窗 N 拍 → `app_adc_wdog_reenable`；`wdog_rearm_max` 超限 → 该相升 TRIP 锁存）
- Modify: `App/Control/Inc/app_fault.h`、`config/software.yaml` + 参数生成（新键）
- Test: `scripts/tests/fault/test_fault_wdog_rearm.c`（新套件 `scripts/tests/fault/`）

**Interfaces:**
- Produces: `void app_fault_wdog_service(void);`（任务上下文 1kHz）
- Produces: `uint16_t app_fault_get_wdog_rearm_count(void);`
- Consumes: `app_adc_wdog_reenable`、`app_adc_get_raw`

- [ ] **Step 1: 写失败测试**

① WDOG 命中 → 注入连续超窗 raw → 断言 rearm 计数与 `wdog_rearm_max` 后升 TRIP；
② 命中前 `wdog_rearm_ms` 未到 → 不重臂；③ raw 未回窗 → 不重臂。

- [ ] **Step 2: 运行，确认失败**

Run: `bash scripts/tests/fault/run.sh`
Expected: FAIL（`app_fault_wdog_service` 未定义）。

- [ ] **Step 3: 实现**（见 Files）

- [ ] **Step 4: 运行，确认通过**

Run: `bash scripts/tests/fault/run.sh`
Expected: PASS。

- [ ] **Step 5: 全量回归 + 提交**

```bash
git add -A && git commit -m "feat(fault): WDOG 状态评估前移（同拍生效）+ 自动重臂与次数上限"
```
> 台架出口判据（Mephisto 无法执行）：注入实测确认 WDOG 会触发。

---

### Task 4: 逐故障动作策略表 + 分级动作 + 台架注入

**Files:**
- Modify: `App/Control/Inc/app_fault.h`（`app_fault_action_t`、`APP_FAULT_OC_TRIP`(1UL<<10)、`app_fault_set_policy/get_policy/get_action/get_limit_scale/inject`）
- Modify: `App/Control/Src/app_fault.c`（`policy[]`、`fault_raise` 按 policy 定 state、`enable` 契约改"仅带动作码拒绝"）
- Modify: `App/Control/Src/app_foc.c`（`app_foc_enable` 拒绝条件改 `latched & action_mask`）
- Modify: `App/Comm/terminal/Src/app_terminal_cmd_diag.c`（`fault policy show|set`、`fault inject <code> on|off`、`fault` 增列）
- Modify: `config/software.yaml` + `scripts/gen_params.py`
- Test: `scripts/tests/fault/test_fault_policy.c`

**Interfaces:**
- Produces: `void app_fault_set_policy(uint32_t code, app_fault_action_t act);` / `app_fault_action_t app_fault_get_policy(uint32_t code);` / `app_fault_action_t app_fault_get_action(void);` / `float app_fault_get_limit_scale(void);` / `int app_fault_inject(uint32_t code, bool on);`
- 默认 policy：`oc_fast=trip, oc_slow=derate, vbus_ov=stop, vbus_uv=stop, adc_timeout=trip, enc_read=warn, oc_trip=trip`（YAML `fault.action.*`）
- `APP_PROTECT_ACTION_EN=0` 时强制全部按 WARN。

- [ ] **Step 1: 写失败测试**

① policy 查表与 YAML 一致；② `get_action` 取最高级；③ WARN 不置 state=FAULT、TRIP 置；④ `inject` 置位后 `get_action` 反映 policy；⑤ 带动作码锁存 → `app_foc_enable` 拒绝；纯 WARN 锁存 → 不拒绝。

- [ ] **Step 2: 运行，确认失败** → `bash scripts/tests/fault/run.sh` FAIL
- [ ] **Step 3: 实现**
- [ ] **Step 4: 运行，确认通过**
- [ ] **Step 5: 全量回归 + 提交**

```bash
git add -A && git commit -m "feat(fault): 逐故障动作策略表（WARN/DERATE/STOP/TRIP）+ 台架注入命令"
```

---

### Task 5: L3 母线两级判据 + STOP/TRIP 分派

**Files:**
- Modify: `App/Control/Src/app_fault.c`（L3 用 `app_analog_signal_vbus_fast()`；`vbus_ov_ms`/`vbus_uv_ms`/`vbus_ov_trip_v` 去抖按 ms）
- Modify: `App/Control/Src/app_foc.c` + `Inc`（`app_foc_request_fault_stop(uint32_t code)`，OFF-桥使能/CALIB/vtest → 返回 −1 降级 TRIP）
- Modify: `config/software.yaml`（`vbus_ov_trip_v: 40.0`、`vbus_ov_ms: 1`、`vbus_uv_ms: 1`、`fault_stop_speed_wait_ms: 500`）
- Test: `scripts/tests/fault/test_fault_vbus_levels.c`

**Interfaces:**
- Produces: `int app_foc_request_fault_stop(uint32_t code);`
- 一级 36V → STOP；二级 40V（无去抖）→ TRIP；9V → STOP；STOP 的 deadline 到期仍在动 → 慢 tick 降级 TRIP。

- [ ] **Step 1: 写失败测试**：① 36V 连续 `vbus_ov_ms` 拍 → action=STOP；② 单拍 40V → action=TRIP；③ 9V → STOP；④ CALIB 下请求 STOP 返回 −1（降级 TRIP）
- [ ] **Step 2: 运行，确认失败**
- [ ] **Step 3: 实现**
- [ ] **Step 4: 运行，确认通过**
- [ ] **Step 5: 全量回归 + 提交**

```bash
git add -A && git commit -m "feat(fault): 母线两级判据（36V STOP / 40V TRIP）与受控停机分派"
```

---

### Task 6: EWDG + 三角心跳

**Files:**
- Create: `Interface/Inc/intf_ewdg.h`、`Driver/hpm_impl/drv_ewdg.c`
- Create: `App/Platform/Src/app_watchdog.c`（喂狗策略）
- Modify: `Driver/hpm_impl/drv_adc.c`（ADC0 ISR 调 `app_watchdog_isr_pulse()`）
- Modify: `App/Logic/app_logic.c`（init 顺序）
- Test: `scripts/tests/fault/test_fault_heartbeat.c`

**Interfaces:**
- Produces: `int intf_ewdg_init(uint32_t timeout_ms, bool reset_on_timeout);` / `void intf_ewdg_refresh(void);`
- Produces: `void app_watchdog_isr_pulse(void);`（ISR：刷新心跳 + 门控喂狗）/ `void app_watchdog_task_pulse(void);`（任务活性计数）
- `ewdg_timeout_ms` 默认 100；`0` = 关闭。先加 `flash_critical_max_us` 计数器实测最长 MIE=0 窗口，再定超时。

- [ ] **Step 1: 写失败测试**：① 任务心跳停 → `app_watchdog_isr_pulse()` 停止喂狗（用 mock 计数喂狗调用）；② 慢环停 → 同；③ 全部正常 → 每拍喂。
- [ ] **Step 2: 运行，确认失败**
- [ ] **Step 3: 实现**（`ewdg_init_interrupt_reset`；驱动注册 `hpm_gptmr_driver_register` 风格）
- [ ] **Step 4: 运行，确认通过**
- [ ] **Step 5: 全量回归 + 提交**

```bash
git add -A && git commit -m "feat(protect): EWDG 兜底 + 三角心跳（ISR/慢环/任务互相监督）"
```
> 台架出口判据：临时 `while(1)` → ≤100ms 复位且复位源可读。

---

### Task 7: DERATE（I²t）+ 退役旧开关

**Files:**
- Modify: `App/Control/Src/app_fault.c`（I²t 预算耗尽 → DERATE；`derate_scale` 默认 0.5）
- Modify: `App/Control/Src/app_foc_current.c`（`app_foc_current_get_i_limit()` 乘 `app_fault_get_limit_scale()`）
- Modify: `config/software.yaml`（`derate_scale: 0.5`；`shutdown_en` 标注退役）
- Modify: `App/Control/Src/app_fault.c`（`shutdown_en` 不再参与 state 决策）
- Test: `scripts/tests/fault/test_fault_derate.c`、同步 `scripts/tests/control` 的 i_limit 用例

**Interfaces:**
- Consumes: Task 4 的 `app_fault_get_limit_scale()`。
- `APP_PROTECT_ACTION_EN` 语义固定为"台架强制全 WARN"。

- [ ] **Step 1: 写失败测试**：① `limit_scale` 与 policy 联动；② 耗尽 → `app_foc_current_get_i_limit()` 下降；③ 恢复 → 回满；④ `shutdown_en` 不再改变 state（回归）
- [ ] **Step 2: 运行，确认失败**
- [ ] **Step 3: 实现**
- [ ] **Step 4: 运行，确认通过**
- [ ] **Step 5: 全量回归 + 提交**

```bash
git add -A && git commit -m "feat(fault): I²t 预算耗尽转 DERATE；退役 shutdown_en 全局开关"
```

---

## 收尾

- 全量：`adc analog control foc encoder identify params` + `layout` + `gen` + `make artifacts`（0 warning）+ `git diff --check`。
- 更新 `AGENTS.md`：分层契约、"1kHz"单位改述、非抢占 ISR 事实、ILM 清单、台架待验证项。
- 整体审查（fresh reviewer），Critical/Important 一轮修复。