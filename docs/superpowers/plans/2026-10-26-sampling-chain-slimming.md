# 采样链精简：NTC/CANID 低频化 + 中断快环路只留 FOC/motion（2026-10-26）

> 状态：**已裁决（2026-10-26，Kaiser）——fault 保留在快环、由 ADC1 中断回调负载**；
> 原方案 A（fault tick 搬入 1kHz 任务）**作废**。当前结论：维持现状（tick 速率=数据率
> 已是最优），若要进一步精简则走 B3。**待 Kaiser 提供 `adc diag` / `profiler dump` 实测占用。**
> 前置：`docs/superpowers/plans/2026-10-26-vbus-sampling-sync.md`（vbus 同源同步，已实施）
> 关联：spec `2026-09-19-adc-sampling-design.md` §9.5

## 1. 需求

- **R1** NTC/CANID 读数不需要高频刷新，放到 FreeRTOS 任务编排中执行。
- **R2** 中断快环路**只留 FOC 与 motion 相关**。
- **R3** 保住 vbus 的已定案收益（PWM 同源触发、同拍、30kHz 新鲜；配对 A/B 证据见 §9.5.2）。
- **R4** 保护节拍红线：`app_fault_tick()` 仍 1kHz、去抖 5 次 = 5ms，语义不变。

## 2. 现状盘点（ISR）

| ISR | 频率 | 用途 | 本方案归属 |
|---|---|---|---|
| ADC0 PMT | 30kHz | 三相电流 + vbus 快通道锁存 + FOC 电流环 | **FOC，保留** |
| **ADC1 SEQ 完成** | **30kHz**（软件按 `pwm_freq/1000` 分频后驱动 fault tick） | NTC/CANID/vbus 序列完成 | **移除**（R2 的主要对象） |
| GPTMR1 CH0 | 15kHz | motion 单拍（位置+速度） | **motion，保留** |
| GPTMR0 CH2 | — | 本批已移除（旧 vbus 1kHz 触发源）；现无使用者 | — |
| PWM0 / PWM1 | 默认关闭 | 仅 debug 命令 `pwm irq` 可开 | 需 `profiler dump` 确认 |
| GPIO0_A/B | 由 app 决定 | 待确认 | 待确认 |
| USB / MCAN / UART | 事件驱动 | 通讯（非周期） | 保留 |

**验证工具（均在 USB 终端，非 UART）**：
- `profiler dump` —— 逐 ISR 的 `hits/Hz`、`last/min/max/avg ns`、`outliers`（**快环路基线与改动后核对就用它**）
- `adc diag` —— ADC 诊断（含本批新增的 ADC1 SEQ 帧率/`vbus_fast` 活性行）
- `adc dump` —— 7 通道原始码/物理量表
- `adc delay <ns>` —— 运行期改触发延时（等价原 `k` 命令）
- `foc` —— 含 `vbus src=… fast=… slow=… seq=… stale=…`

## 3. 硬约束（决定方案形状，勿绕）

1. **每触发 ≤4 槽**（`ADC_SOC_MAX_TRIG_CH_LEN = 4`，HPM5361 feature:108）。ADC0 队列
   `[I_W, I_U, I_V, I_W]` 已满（3 电流 + 1 队尾副本，副本是首槽污染规避的必需项）
   → **vbus 进不了 ADC0 的 PMT 队列**。
2. **一个 ADC 实例只有一种转换模式**（INTF_ADC_MODE_*，硬件级）；**SEQ 只有一个触发源**
   → 一个实例只能有一个速率。
3. 推论：**只要 vbus 需要 PWM 同步的 30kHz，同一实例上的 NTC/CANID 在硬件层就会被同样
   高频"转换"**。软件可以只低频"读取"（现状已如此：`app_adc_slow_process()` 在 1kHz 任务），
   但"转换"在同一实例上无法与 vbus 解耦。
   → 因此 R1 的"读数放到任务里"**现状已满足**；真正要改的是 **R2（去掉 30kHz 的 ADC1 ISR）**，
   以及可选地把 NTC/CANID 的**转换**也从 30kHz 拿下来（方案 B）。

## 4. 方案 A（**已作废**，2026-10-26 Kaiser 裁决）

> Kaiser 裁决：`fault` 保留在快环、由 ADC1 中断回调负载；"如果常态 CPU 占用不大可以维持
> 现状，这比 fault 降速更有利于保护"。故本方案不再实施，ADC1 的 SEQ 中断保留。

### 4.0 一个必须先纠正的前提：tick 速率 ≠ 保护速度

`app_fault_tick()` 的两个低速判据的数据源**都是 1kHz**：

| 判据 | 数据源 | 更新率 |
|---|---|---|
| L2 RMS 过流 | `app_fault_process()`（1kHz io 任务喂 `s_rms[]`） | 1ms |
| L3 母线 OV/UV | `app_analog_signal_read(ADC_CH_V_VBUS)`（1kHz 物理值） | 1ms |

因此把 tick 提到 30kHz：连续 30 次 tick 读到**同一个数**，去抖 5 次 → **0.17ms 即命中**
→ 等效"单次采样即触发"→ 误报率 ×30，而**检测延迟不变**（仍受 1ms 数据更新约束）。
单位契约在代码里三处写明：`app_software_params.h:41`（"L2/L3 去抖次数（1kHz）"）、
`app_fault.h:66`、`config/software.yaml:30`。
**结论**：tick 速率提到数据率之上只缩短持久窗口、不提升保护速度；当前
`分频比 = pwm_freq/1000`（30kHz→1kHz）**恰好等于数据率，是该维度上的最优解**。
若要让保护真正更快，必须提高**数据率**（见方案 O3），而不是 tick 速率。

### 4.1 原方案 A 内容（留档）


### 改动

1. `App/Platform/Src/app_adc.c`
   - ADC1 `seq_cb = NULL`（不再使能 SEQ 完成中断）→ **30kHz ADC1 ISR 消失**；
   - 移除为分频而引入的 `s_slow_cb_divisor / s_slow_cb_counter / adc_slow_adapter`
     （无 ISR 后分频机制不再需要）；`app_adc_cfg_t.slow_cb` 的去留见下；
   - vbus 快通道锁存（ADC0 PMT ISR 内）+ `intf_adc_get_seq_frames` 新鲜度握手不变。
2. `App/Logic/app_logic.c`
   - `app_fault_tick()` 由 1kHz `app_io_step` 调用（位置：紧接 `app_adc_slow_process()`
     与 `app_analog_signal_process()` 之后、`app_fault_process()` 相邻）；
   - tick 前先用 `intf_adc_get_seq_frames(1)` 判断 ADC1 慢链是否推进（**把原先"tick 由
     ADC1 序列完成驱动"所隐含的链存活检测显式化**），未推进时的处理保持与今天同强度
     （仅告警/计数，不新增保护动作）。
3. 主机回归
   - 现有 `scripts/tests/adc/test_app_adc_layout.c` 的"慢回调分频"用例改为断言
     **`seq_cb == NULL`**（ADC1 不使能中断）；
   - 新增 `app_fault` 侧用例：慢链未推进 → tick 的显式检查触发（若该检查放在 app_fault）；
   - 若检查放在 app_logic（无主机套件），则新增一个只测"帧推进判定"的小用例到 adc 套件。

### 达成情况

| 需求 | 达成 |
|---|---|
| R2 快环路只留 FOC/motion | ✅ 30kHz 的 ADC1 ISR 消失；定期 ISR 只剩 ADC0 PMT（FOC）与 GPTMR1 CH0（motion） |
| R1 NTC/CANID 读数在任务 | ✅ 现状即如此（1kHz `app_adc_slow_process`），本次进一步移除其 ISR 依赖 |
| R3 vbus 收益 | ✅ 快通道锁存仍在 ADC0 PMT ISR 内，与电流同拍 |
| R4 保护节拍 | ✅ fault tick 仍 1kHz、5 次去抖 = 5ms（改的是调用点，不是阈值/计数） |

### 代价与残余

- NTC/CANID 的**硬件转换**仍随 vbus 在 30kHz（ADC1 占空 0.6%→17%）。零 CPU、零 ISR，
  仅 ADC1 的转换窗口被占用；若 Kaiser 要求"转换也不高频"→ 方案 B。
- 移除 ADC1 ISR 后，SEQ 完全硬件自主。若 SEQ 停摆：vbus 侧由 `stale` 计数发现，
  NTC/CANID 侧由新增的显式慢链检查发现（等价于今天的检测能力）。

### 附带收益：cal rl 剩余项的判别

方案 A 移除了"两臂共有的 30kHz ADC1 活动"（spec §9.5.3 剩余候选 1），因此它就是该开放项
的判别实验：实施后复测 `cal rl`——
- `Rs_eff` 回到 ~0.162、`Ld` 回到 ~121 µH → **定位到 ADC1 30kHz 活动**（下一步再查是
  并发转换耦合还是 ISR 干扰）；
- 否则归入会话间工况差异（停机位/接线/接触电阻），**该项目结案**。

## 4.2 tick 速率选项（CPU 与保护语义）

`app_fault_tick()` 单次成本（代码级估算，且位于 **XIP flash**、含若干 flash 间接调用：
`app_encoder_get_error_count`、`get_rms` 间接调用）：≈450~500 cycles ≈ **1µs @480MHz**。

| 选项 | 内容 | 保护语义 | CPU | 判定 |
|---|---|---|---|---|
| **O1** | **维持现状**：ADC1 SEQ 30kHz ISR + 分频 30 → tick 1kHz | 与改动前**逐位一致** | tick 侧 ≈0.1%；ADC1 ISR 侧 ≈1~1.8%（vbus 30kHz 同步的代价） | **推荐** |
| O2 | 去分频 → tick 30kHz；`slow_debounce` 5→150、`adc_stall_ms` ×30 同步重标 | 持久时间名义不变，但超时/误报窗口实质收紧（同一采样被重复计数） | **+2.7%**（XIP 下更高） | 不推荐（零收益 + 属保护逻辑改动） |
| O3 | **真正提速**：L3 换用快通道 vbus（30kHz 新鲜）+ 去抖按 ms 重标；L2 若需 30kHz 则在 PMT ISR 内累加 RMS | 检测延迟 1ms → 33µs(vbus) / 31µs(RMS) | 需在 ISR 内新增累加 | 需**单独立项**（保护逻辑改动，Kaiser 已划为最后做） |

**实测方法（USB 终端，请以实测为准，勿用估算下结论）**：
- `adc diag` → `[ADC] ADC1: … isr_max=… cyc isr_cpu=x.x% rate=… Hz`（驱动 `isr_total_cycles`
  的墙钟占用，直接测量；含分频后的 fault tick 载荷）
- `profiler dump` → ADC1 slot 的 `avg/max ns` 与 `Hz`、`outliers`

## 5. 方案 B（可选，需裁决）：连 NTC/CANID 的转换也不高频

三条路线，均需触及其他域：

| 路线 | 做法 | 收益 | 代价 / 风险 |
|---|---|---|---|
| **B1** | 腾出 ADC0 一个 PMT 槽：三相电流改两相重构，队列 `[I_U, I_V, VBUS, I_U]`（槽0=队尾副本规则仍满足） | vbus **真正同队列同拍**；电流转换 4→3 槽，低侧窗口更宽（d_max ↑） | **放弃第三路独立采样** → 硬件 WDOG 逐相保护、零点标定、诊断、`cal` 路径全部要改 → **触碰保护红线** |
| **B2** | vbus 改 ADC0 第二 PMT 队列（TRG1A，2 槽 `[VBUS, VBUS]`）；ADC1 SEQ 降到低频跑 NTC/CANID | NTC/CANID 转换低频 | 同实例两队列共享转换器、**插入顺序不可控**（vbus 可能抢在电流前吃掉低侧采样窗口）；驱动需支持多 PMT 队列；每周期多一次 TRIG_CMPT → **不推荐** |
| **B3** | ADC1 SEQ 只留 `[VBUS, VBUS]`；NTC/CANID 用 **BUS_RESULT oneshot** 低频读（10Hz） | 最干净：vbus 30kHz 同步、NTC/CANID 10Hz、零 ISR、队列不改 | **未验证**：实例处于 SEQ 模式时 BUS_RESULT oneshot 是否可用、是否置 `read_cflict`/扰动 PRD_RESULT（手册 §53.4.4）。需先做寄存器实验 |

**B3 的前置实验（建议先做，成本低）**：在 ADC1 SEQ 运行中，用 debug 命令对 NTC0 做一次
oneshot 读，观察 (a) 是否返回合理值、(b) `PRD_RESULT[6]`(vbus) 是否被扰动、(c) `INT_STS`
是否出现冲突标志。若三项通过 → B3 可行。

## 6. 分阶段建议

1. **第 1 步（本批）**：方案 A —— 满足 R1/R2/R3/R4。
2. **第 2 步**：`profiler dump` 前后对比（ADC1 slot 应从 ~30kHz 消失）+ `cal rl` ×2
   → 判定 spec §9.5.3 的剩余项。
3. **第 3 步（按需）**：若要求"转换也不高频" → 做 B3 前置实验，通过则实施 B3；否则停在 A。

## 7. 风险与回退

- **保护路径上下文变更**：fault tick 由 ISR → 1kHz 任务（节拍/阈值/去抖计数不变）。
  这是 Kaiser 红线（"保护逻辑最后做"）唯一的触碰点，**需显式确认**；不确认则本方案
  无法达成 R2（因为 fault tick 目前寄生在 ADC1 的 SEQ 中断上）。
- 单一提交可回退：`git checkout` 后重新烧录即恢复。
- 无 flash 布局 / 参数 ABI 变化。