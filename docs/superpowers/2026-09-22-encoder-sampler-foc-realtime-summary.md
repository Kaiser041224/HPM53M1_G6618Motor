# 编码器独立采样器 + FOC 实时域解耦 交付摘要（2026-09-22）

> 本文件记录本次实现的交付物、可复现验证结果、Ozone 工作流与未上板验证的约束。
> 设计依据：`docs/superpowers/specs/2026-09-22-encoder-sampler-foc-realtime-design.md`
> 实施计划：`docs/superpowers/plans/2026-09-22-encoder-sampler-foc-realtime.md`

---

## 1. 交付物

### 新增

| 文件 | 作用 |
| :--- | :--- |
| `App/Algorithm/Inc/algo_encoder_snapshot.h` | 编码器一致快照契约（seqlock/坏帧策略） |
| `App/Algorithm/Src/algo_encoder_snapshot.c` | 纯算法实现（零硬件依赖，可宿主测试） |
| `App/Debug/Inc/app_debug_foc.h` | Ozone FOC 调试结构（请求域 + 状态域） |
| `App/Debug/Src/app_debug_foc.c` | 状态刷新 + 有界命令处理 |
| `scripts/tests/encoder/run.sh` | 编码器/应用层宿主测试入口（两遍 -O1 / -O2 -ffast-math） |
| `scripts/tests/encoder/test_util.h` | 断言宏 |
| `scripts/tests/encoder/test_main.c` | 测试入口 |
| `scripts/tests/encoder/test_algo_encoder_snapshot.c` | 快照/坏帧/失败/陈旧/复用 用例（61 检查） |
| `scripts/tests/encoder/test_app_encoder_ownership.c` | mock SPI 所有权/坏帧发布 用例（14 检查） |
| `scripts/tests/encoder/mock_platform.c` | 编码器/SPI/参数/时钟/GPTMR 替身 |

### 修改

| 文件 | 变更 |
| :--- | :--- |
| `App/Platform/Inc/app_encoder.h` / `Src/app_encoder.c` | 快照 API；SPI3 单一所有者；坏帧不发布修复；采样时间戳；`app_encoder_sampler_start()` |
| `App/Platform/Inc/app_gptmr.h` / `Src/app_gptmr.c` | 新增 `APP_GPTMR_CH_3`（GPTMR1 CH3 @12.5kHz；不占 GPTMR0 CH2/全局 2） |
| `App/Control/Src/app_foc.c` | ISR 只读快照、复用 + 时间戳 dt、统一出口计时、仅 FAULT 门控、ISR 紧急关桥、vtest 移入 ISR |
| `App/Control/Src/app_foc_current.c` / `Inc/app_foc_current.h` | `v_max` 改 `foc_modulation_vmax`（修 `/1.5`）；vtest fresh 版本（ISR，真实 dt + 新鲜电流） |
| `App/Algorithm/FOC/Inc/foc_modulation.h` / `Src/foc_modulation.c` | 新增 `foc_modulation_vmax()` = `(2D−1)·vbus/√3` |
| `Driver/hpm_impl/drv_spi.c` | 单帧等待加基于 `intf_clock_get_cycle()` 的 20µs 硬期限（时间界，非循环界） |
| `App/Debug/Src/app_debug_encoder.c` / `Inc/app_debug_encoder.h` | passive/energized 字段分离；电气标定占位 |
| `App/Logic/app_logic.c` | 启动采样器；`app_debug_foc` 接线；台架模式旁路 USB/终端 |
| `Makefile` | 新增 `APP_DEFINES`（如 `-DAPP_BENCH_DEBUG_MODE=1`） |
| `CMakeLists.txt` | 新源文件登记 |
| `scripts/tests/foc/test_foc_modulation.c` | 新增 `foc_modulation_vmax` 用例（+6 检查） |

**未改动**：`config/*.yaml`（用户脏值 `kp=0.3 / ki=100 / i_trip=0 / speed=0` 原样保留）。
**无 git commit**。

---

## 2. 可复现验证结果（本次实际运行输出）

### 宿主测试

```
$ bash scripts/tests/encoder/run.sh
── pass 1: -O1（常规）
Encoder/app host tests
75 checks, 0 failures
── pass 2: -O2 -ffast-math
Encoder/app host tests
75 checks, 0 failures

$ bash scripts/tests/foc/run.sh
── pass 1: -O1（常规）
FOC host tests
272 checks, 0 failures
── pass 2: -O2 -ffast-math
FOC host tests
272 checks, 0 failures
```

### 编译

| 目标 | 命令 | 结果 |
| :--- | :--- | :--- |
| Debug `-Og` | `make build OPT_LEVEL_DBG=-Og` | BUILD SUCCESS（FLASH 17.27%） |
| Release `-O3` | `make CMAKE_BUILD_TYPE=Release OPT_LEVEL_REL=-O3 build` | BUILD SUCCESS（FLASH 19.46%） |
| 台架模式 | `make APP_DEFINES=-DAPP_BENCH_DEBUG_MODE=1 OPT_LEVEL_DBG=-Og build` | BUILD SUCCESS |

> 最终产物由最后一次 `-Og` Debug 构建生成（`output/HPM53M1_G6618Motor.elf`）。
> **未上板**：上述仅证明编译与宿主逻辑，不构成硬件功能/实时性成功的声明。

---

## 3. 设计速查表

| 主题 | 决策 |
| :--- | :--- |
| 采样节拍 | GPTMR1 CH3 @12.5kHz（全局 7） |
| PLIC | GPTMR=3 > ADC0=2（沿用 `drv_gptmr.c`/`drv_adc.c` 默认；采样 ISR 可抢占 FOC ISR） |
| 单一 SPI3 所有者 | 采样 ISR；`app_encoder_sampler_claim()` 后运行期读 API 只返回快照 |
| 快照一致性 | seqlock；ISR 读最多 1 次重试、主循环 8 次，绝不无限自旋 |
| 坏帧 | 拒绝值不发布（修复原缺陷）；连续失败/跳变 → 快照 invalid |
| 25kHz 复用 | `seq` 判新样本；`timestamp_cycles` 差算速度 dt；复用时保持 θe/ωe |
| 新鲜度 | 采样周期 80µs + 余量 40µs = 120µs；超龄判换相角不可信 |
| ISR 计时 | 单一出口统一 `g_foc_isr_cycles` + 预算检查（含故障路径） |
| 故障门控 | 仅 `APP_FAULT_STATE_FAULT`（或 ISR 请求）停机；WARNING 不停机 |
| 紧急停机 | ISR 直接 `app_3phase_inverter_emergency_stop()`，独立于主循环 |
| PI 圆限幅 | `v_max = (2·duty_max − 1)·v_bus / √3` |
| 饱和处理 | 去掉 bang-bang 置零；保留 PI 抗饱和 + `saturated`/`v_scale` 观测 |
| vtest | 仅在 ADC0 ISR 执行（新鲜电流 + 真实 dt + 过流保护） |
| 输出所有者 | 实时占空比只由 ADC0 ISR 写；主循环只发请求/编排 |
| 调试 | `g_app_debug_foc`（`.noncacheable.bss`），命令 ack/result |
| 自检 | passive（不使能桥）与 energized（通电标定）字段分离 |

---

## 4. Ozone 精确工作流

### 4.1 观测变量（Watch 窗口按符号名添加）

`g_app_debug_foc`（结构体）：`state / enabled / fault_codes / fault_latched / tripped /
saturated / isr_cycles / isr_cycles_max / isr_overruns / isr_run_count /
enc_seq / enc_age_us / enc_age_cycles / enc_errors / enc_jumps /
i_d_a / i_q_a / i_d_avg_a / i_q_avg_a / i_d_ref_a / i_q_ref_a /
duty_u / duty_v / duty_w / v_bus_v / v_scale / theta_e_rad / omega_e_rad_s / tick_count`。

单独变量：`g_foc_isr_cycles`、`g_foc_isr_cycles_max`、`g_foc_isr_overruns`、`g_foc_enc_age_us`、
`g_foc_enc_seq`、`g_foc_fault_request`、`g_enc_passive_ok`、`g_enc_passive_spi_hz`、
`g_enc_energized_ok`、`g_enc_energized_not_run`。

> 结构体在 `.noncacheable.bss`，启动清零、D-Cache 不影响；建议 Watch 展开而非整体 Plot。

### 4.2 一次性命令（Write 窗口）

写 `g_app_debug_foc.command = <值>`，随后 main 循环 tick 执行并回写：
`ack_result`（0 成功）与 `ack_sequence`（+1）；命令处理完成后 `command` 自动回 `0`。

| 命令 | 值 | arg0 / arg1 |
| :--- | :--- | :--- |
| NONE | 0 | — |
| ENABLE | 1 | —（先清零 iq/id 目标，再 `app_foc_enable`） |
| DISABLE | 2 | —（先清零目标，再 `app_foc_disable`） |
| CLEAR_FAULT | 3 | — |
| VTEST | 4 | arg0 = 电压 ×1000 [V]；arg1 = θe ×10000 [rad]（需已使能） |
| CAL_ENCODER | 5 | —（`app_motor_identify_start`，需 READY/RUN） |
| CAL_ABORT | 6 | —（`app_motor_identify_abort`） |
| PASSIVE_SELFTEST | 7 | —（启动运行期健康自检；不触碰 SPI3，100ms 出结果） |

### 4.3 运行期给定与紧急停机（Debug 结构请求域，Ozone 直接写）

| 字段 | 写值 | 行为 |
| :--- | :--- | :--- |
| `iq_target_a` | float [A] | 主循环仅在 READY/RUN 且值**变化**时经 `app_foc_set_iq_ref` 限幅应用（不覆盖终端设定） |
| `id_target_a` | float [A] | 同上，经 `app_foc_set_id_ref` |
| `estop_request` | 写 1 | **ADC ISR 直接消费**（无需主循环）：立即 `emergency_stop` + 锁存 `isr_inhibited`；`disable` 清零 |

> 紧急停机邮箱由 Control 持有指针（`app_foc_register_estop_request(&g_app_debug_foc.estop_request)`
> 在 `app_debug_foc_init` 绑定）；Ozone 写 `estop_request=1` 后 ISR 下一拍即可停机。
> 回读 `estop_ack`（= `g_foc_fault_request`）与 `isr_inhibited` 确认已触发。
> 若无需外部邮箱，Control 内部 `g_foc_fault_request` 亦会因故障/过流被 ISR 置位。

### 4.4 电气标定状态（只读）

`cal_active / cal_done / cal_failed / cal_fail_reason / cal_progress / cal_offset_rad /
cal_direction / cal_quality`（源自 `app_motor_identify_result_t`）。标定推进集中在
`app_debug_foc_tick()`（1kHz），无 Terminal 依赖。

### 4.5 采样器观测（只读）

`enc_read_fail / enc_isr_cycles / enc_isr_cycles_max`；单独变量
`g_encoder_isr_cycles / g_encoder_isr_cycles_max / g_encoder_sample_count /
g_encoder_read_fail_count`。

### 4.3 推荐上电检查顺序（台架）

1. 外部电源供电，确认 +3.3V/+5V 稳定（**勿用调试器供电**，见 AGENTS.md）。
2. `state==0(OFF)`、`enc_passive_ok==1`、`enc_seq` 递增、`enc_age_us` 约 ≤80µs。
3. 写 `ENABLE` → `state==READY`；`i_d/i_q≈0`；`duty=0.5`。
4. 零给定下观察 `i_d_avg/i_q_avg` 应 ≈0（验证电流符号）。
5. 小给定 `i_q_ref`（经终端 `foc iq` 或参数）→ 观察 `omega_e_rad_s`、`isr_cycles`。
6. 任意异常：`DISABLE` → `CLEAR_FAULT`，再排查 `fault_codes`/`tripped`。

---

## 5. 未决 / 未上板验证的硬件约束（诚实清单）

1. **一切实时性结论未上板**：GPTMR 抢占 ADC0、12.5kHz SPI 采样时序、ISR 预算均需台架示波确认。
2. **PWM 三相同步提交（高功率阻塞项，未实现）**：SDK 支持 `PWM_SHCR_SHLKEN` +
   `pwm_shadow_register_lock()` + 写 CMP + `pwm_issue_shadow_register_lock_event()`（`on_shlk`）。
   现驱动用 `on_modify`（逐相立即生效）。**不做“数条指令偏斜”淡化**：ADC0 写 6 次 CMP 期间可被
   GPTMR 采样 ISR（优先级更高）抢占，偏斜可达 GPTMR ISR 时长量级（**≥11µs**，25kHz 下达 25%+ 周期）。
   **未做“仅屏蔽 IRQ 覆盖六次写”**（SDK 锁时序未验证，不猜测）。上板验收 = 示波器确认同一重载点提交；
   在此之前禁止带载/大电流运行。
3. **控制层有效占空比上限（初步）**：`D_eff=min(user duty_max, 0.70)`，对 PI 圆限幅/调制/vtest
   一致生效，暴露为 `g_foc_duty_max_effective` / Debug `duty_max_eff`；**0.70 为四槽 PMT 时序余量
   的初步保守值，必须示波确认后调整**。带载验收前不得放宽。
4. **坏帧跳变阈值**：KTH7823 帧无 CRC，5°机械/连续 3 次为启发式；需按实际最高转速与共振台架复核。
5. **电气标定已接线（非占位）**：`CAL_ENCODER` 经 `app_motor_identify_start/run_once/abort`
   驱动，状态/进度/结果暴露于 Debug 结构；被动自检为真实寄存器读（`PASSIVE_SELFTEST`）。
   `energized_ok/energized_not_run` 保留为字段（供结果标注），**未实现桩函数已删除**；
   实际通电标定走辨识流程。
6. **用户配置保留**：`i_trip_a=0`（快速过流关闭）意味着调试期仅依赖 WDOG 与 L2；台架需显式设置
   安全阈值（经终端参数或 Debug 命令，测试结束不写回 YAML）。
7. **状态字竞态**：状态/给定/角源事务已用 `intf_sys_irq_save/restore` 有界临界区保护；
   10ms 栅极供电等待在临界区之外。ISR 每拍有界只读 `s_estop_request` 指针。
8. **台架模式**：`APP_BENCH_DEBUG_MODE=1` 旁路 USB/终端，失去终端恢复路径，仅限台架；默认关闭。
9. **不复述既往错误结论**：此前“SPI 超时 = 2 帧 × 3 循环”因单帧早退不成立；EMI 为坏帧根因
   亦未被证实。本实现按“帧无校验、坏值可能到达”做防御，不声称根因。
10. **陈旧母线电压（残余，未修）**：`v_bus` 来自 ADC1 慢速通道（1kHz）。`app_analog_signal_read`
    未携带年龄；ADC1 停摆时可能返回上一次仍“合理”的值，FOC 无法识别。`app_adc_is_valid()` 仅覆盖
    PMT 三相电流。彻底修复需在模拟量层加时间戳/年龄（本次未做）。台架应确认 ADC1 慢帧活性。
11. **良性跨域读（残余）**：`app_foc_run_body` 写 `s_i_q_ref` 未在临界区；ISR 读该 32 位 float
    可能得到上一拍值（原子写，最坏一拍旧值）。`g_foc_current_snapshot` 与 `g_app_debug_foc`
    的多字段读非原子（逐字段 32 位），仅影响观测/一拍限速判据，不构成输出错误。

---

## 7. 第二轮修复覆盖（对照独立评审发现）

| 独立发现 | 处理 | 证据 |
| :--- | :--- | :--- |
| Debug 无 writable iq/id 目标 | 新增 `iq_target_a/id_target_a`，READY/RUN 且**变化**时应用；enable/disable 清零 | 代码 + host test 不覆盖该层（编译验证） |
| CAL_ENCODER 占位 -2 | 改为 `app_motor_identify_start`，1kHz 集中于 `app_debug_foc_tick`，暴露状态/进度/结果 | 编译 + 流程接线 |
| disable 置 OFF 在最后、复位 PI 无临界 | OFF 先置（临界区），PI 复位在临界区外；enable 10ms 等待在临界区外 | 代码审查 |
| enter/exit calib、set_angle_source 竞态 | 全部用 `intf_sys_irq_save/restore` 有界临界区 | 代码审查 |
| ISR 缺 `app_fault_get_state` FAULT 检查 | ISR 入口即时门控 + `s_isr_inhibited` 锁存，后续拍不再输出 | 代码审查 |
| OFF 早退跳过计时 | 统一 `isr_exit` 标签：OFF/故障/抑制均计时与预算检查 | 代码审查 |
| vtest 用了编码器样本 dt | ISR 新增 `isr_dt_s`（ADC 实测间隔）并传入 vtest；编码器样本 dt 仅用于 ωe | host test §6 |
| 运行期 read_reg/set_direction/set_zero_mtp 仍碰转子 SPI | claim 后对转子一律返回 -1（不触碰 SPI3） | encoder host test §5 |
| snapshot_read 返回值被忽略 | 非一致读时强制 `out->valid=false` | 代码审查 |
| GPTMR 优先级/失败计数/耗时观测 | 新增 `g_encoder_isr_cycles(_max)/sample_count/read_fail_count` 并接入 Debug 结构 | 代码 + Ozone 表 |
| 紧急停机仅 Control 全局 | 新增 Control 指针邮箱 `app_foc_register_estop_request`，Debug 绑定；ISR 直接消费 | 代码 + Ozone §4.3 |
| 当前控制 mock 集成测试 | 新增 `scripts/tests/control/`（PI 限幅/vbus 保护/vtest 跳闸与计时） | 25 checks, 0 failures |

> 仍需上板验证：PLIC 抢占时序、12.5kHz SPI 采样、ISR 预算、PWM 三相同步、编码器跳变阈值。

---

## 6. 与硬件采样的关键推导（供复核）

- 采样窗口：`duty_max=D` → 相间 span 上限 `(2D−1)·v_bus`；平衡正弦 span=`√3·A`
  → 相电压峰值上限 `A_max=(2D−1)·v_bus/√3`。`D=0.885` → `span_max=0.77·v_bus`。
- 编码器新鲜度：采样 80µs、FOC 40µs；`max_age=120µs` 允许最多一次复用；
  超龄即判换相角不可信，避免陈旧角进换相。
