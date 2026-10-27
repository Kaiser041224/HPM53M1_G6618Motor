# VBUS 采样优化：与电流采样同源同步（2026-10-26）

> 状态：**已实施**（Kaiser 裁决：PWM 触发 vbus ADC 采样 / 不使用 PMT / 保留 ADC0 现有队列 / duty 用快 raw + 轻量滤波）
> 主机回归：新增 `scripts/tests/adc/`（52 checks）、`scripts/tests/analog/`（15 checks）+ control `test_foc_vbus_fast_source`；
> 全量套件 0 failures；固件 BUILD SUCCESS（FLASH 24.77% / ILM 22.44%）。
> 台架回归（2026-10-26）：**未收敛** —— cal rl 阶梯 2kHz→754Hz 塌陷；已加
> `g_foc_vbus_fast_en`（默认 0）作单变量 A/B，并新增终端 `foc` 的 `vbus src=` 观测行。
> 详见 spec §9.5.1。
> 关联：`docs/superpowers/specs/2026-09-19-adc-sampling-design.md` §9.1 / §9.2 / §9.4

## 1. 问题定位（已核实事实）

| # | 事实 | 出处 |
|---|---|---|
| 1 | ADC0 PMT 4 槽 `[I_W, I_U, I_V, I_W]`，PWM1 CMP10 触发，30kHz | `app_adc.c:229-234` |
| 2 | ADC0 回调只用 slot1..3，slot0 为牺牲位（吸收首槽污染） | `app_adc.c:161-167` |
| 3 | ADC1 SEQ 5 项，GPTMR0 CH2 1kHz 触发，无 DMA，读 `PRD_RESULTx` | `app_adc.c:249-268` |
| 4 | **ADC1 SEQ 完成中断实际开启**，`app_fault_tick()` 在 ADC1 ISR 内跑（与 spec §9.2 "无 ISR" 矛盾） | `app_logic.c:181` → `app_adc.c:254` → `drv_adc.c:758` |
| 5 | `USE_NONVECTOR_MODE=1` + `DISABLE_IRQ_PREEMPTIVE=1` → **ISR 不可抢占**（`port.c:336` 不重开 MIE） | `CMakeLists.txt:46-47` |
| 6 | vbus 物理值 1kHz 算；30kHz FOC ISR 复用同一值 30~60 拍 | `app_foc.c:546`、`app_analog_signal.c:222` |
| 7 | `app_io_step` 中 `app_analog_signal_process()` 在 `app_adc_slow_process()` **之前** → 慢通道物理量恒定落后 1 拍 | `app_logic.c:329`/`:348` |
| 8 | `PRD_RESULT` 为纯 16-bit RO，无 VALID/cycle-bit/序号 → 无法判新鲜 | `soc/HPM5300/ip/hpm_adc16_regs.h:465` |
| 9 | vbus 滤波为 `NONE`，spec §8 却写 LPF 100Hz | `app_analog_signal.c:60` vs spec `:160` |
| 10 | GPTMR 无预分频 `reload=clk/f`；PWM `reload=clk/f-1`（截断） | `drv_gptmr.c:272`、`drv_hrpwm.c:698` |
| 11 | TRGM 每输出独立选源 → 一源多目标（扇出）天然支持 | `drv_trgm.c:20-81` |
| 12 | **每触发 ≤4 槽**（硬件），ADC0 已满 → vbus 进不了同一 PMT 队列 | `HPM5361/hpm_soc_feature.h:108` |

## 2. 可能原因（排序）

1. **采样时刻在 PWM 载波上游走（推断，可量化）**：PWM=160MHz/5333=**30001.87Hz**，GPTMR0=**1000.000Hz**，非整比 → 相位每 ms 前移 0.062µs，扫满周期 ≈535ms → **≈1.9Hz 拍频**把开关纹波做成"母线波动"。会污染 `cal rl` 的电压重建（`app_foc_current.c:896-898`）。**待实测**（打印 mot0/gptmr0 频率 + 抓 vbus 时序）。
2. **vbus 在 FOC 内最坏 ~2ms 陈旧**（事实 6+7）→ duty/`v_max` 用旧电压。
3. **无时效判定**（事实 8；2026-09-22 summary #10 记为残余未修）。
4. **无滤波**（事实 9）。
5. **PMT 首槽 S/H 建立不足**（2026-09-19 实测硬件行为）：现方案只保护槽0；槽1..3 干净性来自观测而非结构保证。
6. **跨实例共享 PTRGI 交叉触发**：实例首初始化已全清 QUEUE_EN，但清理上界 `ADC_PMT_MAX_TRIG=11` 漏掉索引 11（`drv_adc.c:581`/`:661`）。
7. ADC1 SEQ 无启动丢弃（PMT 有 8 帧）——影响有限。

**已排除**：ADC1 IP 读/周期模式问题（§9.2 已解决）；触发风暴与 vbus 的直接因果；母线真实波动（须先排除 1/2）。

## 3. 方案：ADC1 SEQ 与电流同源触发

约束：每触发 ≤4 槽且 ADC0 已满；槽0 声明通道必须等于队尾通道（否则复现历史 bug）；ISR 不可抢占（不做新 30kHz ISR）；保护节拍红线。

```
PWM1 CMP10（谷底 + trigger_delay_ns）
  ├─→ TRGM PTRGI0A ─→ ADC0 PMT  [I_W, I_U, I_V, I_W]      （不变）
  └─→ TRGM ADC1_STRGI ─→ ADC1 SEQ [VBUS, NTC0, NTC1, CANID, VBUS]
```

- 首/末项同为 VBUS：槽0 污染自吸收（被污染时内容仍是 vbus 自身），末项提供本帧新鲜值。
- VBUS 置于首项 → 触发后 ≈1.15µs 完成，早于 ADC0 PMT（≈4.6µs）。
- ADC0 PMT ISR 内读 ADC1 `PRD_RESULT[6]` → **与电流同拍（≤1 帧 = 33µs 相位相同）**。
- 移除 GPTMR0 CH2 → ADC1_STRGI 链路。
- **保护节拍**：保留 ADC1 SEQ ISR，驱动层按 `pwm_freq_hz/1000` 分频回调 → `app_fault_tick()` 仍 1kHz，语义逐位不变。
  - 副作用可控：ADC1 SEQ 完成（≈5.8µs）晚于 ADC0 PMT（≈4.6µs），而 FOC ISR 跑 ~7µs 且执行期 MIE 关闭 → 30kHz 空转 ISR 落在 FOC ISR 影子下，不引入可测抖动。
  - 后续保护阶段可改为「GPTMR0 CH2 中断驱动 fault tick + 显式新鲜度检查」，届时去掉该 ISR。

## 4. 改动清单

| 文件 | 改动 |
|---|---|
| `Interface/intf_adc.h` | cfg 增 `seq_cb_every`；新增 `intf_adc_get_seq_frames()` |
| `Driver/hpm_impl/drv_adc.c` | SEQ 帧计数（每帧，回调前）+ 回调分频；补齐 trig 清理上界（索引 11） |
| `App/Platform/Src/app_adc.c` | ADC1 序列重排；TRGM 扇出；移除 GPTMR0 链路；PMT ISR 锁存快 vbus + 新鲜度；新 API |
| `App/Platform/Inc/app_adc.h` | 快通道 vbus API + 契约文档 |
| `App/Platform/Src/app_analog_signal.c/.h` | 快通道 `raw→V_bus→LPF(2kHz@30kHz)`；`app_analog_signal_vbus_fast()` |
| `App/Control/Src/app_foc.c` | ISR v_bus 优先快通道，失败回退 1kHz 缓存 |
| `App/Logic/app_logic.c` | `app_adc_slow_process()` 前移到 `app_analog_signal_process()` 之前 |
| `scripts/tests/adc/**`（新） | app_adc 主机覆盖（队列形态/触发扇出/快 vbus/分频） |
| `scripts/tests/analog/**`（新） | app_analog_signal 主机覆盖（换算/滤波采样率/路径隔离） |
| spec + `AGENTS.md` | 记录新机制、首槽规则、§9.2 "无 ISR" 更正、实测结论 |

## 5. 测试清单（TDD，先 RED）

**`scripts/tests/adc/`**（编译真实 `app_adc.c`，mock intf_adc/intf_hrpwm/intf_trgm/app_hardware_params）

| ID | 用例 | 名称（会使它失败的生产改动） |
|---|---|---|
| T1 | `test_adc_pmt_queue_tail_duplicate` | ADC0 队列首槽 == 末槽（改队列顺序破坏自吸收 → 历史 "V_VBUS 读数 73V"） |
| T2 | `test_adc_pmt_slot_read_mapping` | 回调 values[1..3] → I_U/I_V/I_W，首槽丢弃 |
| T3 | `test_adc_seq_vbus_first_and_last` | ADC1 序列首末项均为 VBUS，次项 NTC0 |
| T4 | `test_adc_trigger_fanout` | 存在 `PWM1_CH10REF→PTRGI0A` 与 `PWM1_CH10REF→ADC1_STRGI`，且无 `GPTMR0_OUT2→ADC1_STRGI` |
| T5 | `test_adc_vbus_fast_latch` | SEQ 帧推进时快通道 raw 更新、序列号递增 |
| T6 | `test_adc_vbus_fast_stale` | 无新 SEQ 帧时不更新 raw/序列号，陈旧计数递增 |
| T7 | `test_adc_slow_cb_decimation` | 30kHz 下每 30 帧回调一次（slow_cb 实际 ≈1kHz） |

**`scripts/tests/analog/`**（编译真实 `app_analog_signal.c` + 真实 `algo_filter.c`）

| ID | 用例 | 名称 |
|---|---|---|
| T8 | `test_analog_vbus_fast_scale` | 快通道换算与 1kHz 路径同口径（v_per_volt） |
| T9 | `test_analog_vbus_fast_filter_rate` | LPF α 按 **pwm_freq(30kHz)** 标定：`1−y2/y1 ≈ 0.295`，显著异于 1kHz 的 0.926 |
| T10 | `test_analog_vbus_fast_isolates_slow` | 快通道调用不改变 `app_analog_signal_read(V_VBUS)` |
| T11 | `test_analog_vbus_fast_unavailable` | 无首帧时返回 NAN（供 FOC 回退） |

**control 套件扩展**

| ID | 用例 | 名称 |
|---|---|---|
| T12 | `test_foc_vbus_fast_source` | FOC ISR 优先取快通道；不可用时回退 1kHz 缓存值 |

**无法主机覆盖（列为未覆盖项）**：`app_io_step` 顺序调整（审查 + 台架）；实际转换时序/相位（台架示波 + 时序抓取）。

## 6. 台架验证与判据

1. 打印 `clock_get_frequency(clock_mot0)` / `clock_get_frequency(clock_gptmr0)` 与实际 reload → 证实/否定原因 1 的 1.9Hz 拍频。
2. 抓 vbus raw 时序 10~60s：改前应见 ~1.9Hz 调制（若原因 1 成立），改后应消失。
3. `foc` 新增快通道状态（新鲜/陈旧计数）；静止 + `motion speed 5` 比较 duty/sat 计数/i_q 跟踪。
4. 回归：control / foc / encoder / identify / params / adc / analog 全套 + `d`/`p` 通道表。

## 7. 风险与回退

- 相位钉死后纹波变为「固定相位采样」：可能出现 duty 相关固定偏置 → `k` 扫 `trigger_delay_ns` 找最小值点。
- 快 raw 的 LPF 截止 2kHz（τ≈80µs），母线阶跃响应略慢；系数与截止均为可调 #define。
- 单一提交可整体回退；无 flash 布局 / 参数 ABI 变化。