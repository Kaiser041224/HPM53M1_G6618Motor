# 编码器独立采样器 + FOC 实时域解耦 设计（2026-09-22）

> 状态：已批准（用户 2026-09-22 明确“已批准设计，开始实现”）。
> 本文档固化批准范围、架构决策、安全边界与未决项，作为 `2026-09-22-encoder-sampler-foc-realtime.md`
> 实施计划的依据。

**目标（一句话）**：把转子编码器 SPI 采样从 25kHz ADC 完成中断中拆出，迁到独立的
12.5kHz GPTMR 中断（PLIC 优先级高于 ADC0=2），建立单一所有者 + 一致快照（seqlock，ISR 侧
绝不自旋），并顺带修正电流控制器的限幅/饱和/状态竞态与调试接口，使 25kHz FOC 只消费快照、
时间基准统一、故障/模式切换可验证。

---

## 1. 背景与证据（截至 2026-09-22）

| 现象 | 证据/来源 | 结论 |
| :--- | :--- | :--- |
| 坏帧“采样保持”逻辑实际会把坏原始值发布出去 | `app_encoder.c:138`：`read_raw(&s_rotor_raw)` 先写入，跳变判定失败后只保持 `s_rotor_prev_raw`，未回滚 `s_rotor_raw` | **确认缺陷**：消费方读到的 `s_rotor_raw` 已是坏值；`jump_count` 被记，但发布值被污染 |
| 12.5kHz 采样 > 25kHz 消费可复用同一快照 | 需求；`app_foc.c:307` 当前用相邻 FOC 序号判“采样停摆” | 25kHz FOC 每 2 拍才可能看到新样本，必须用**样本时间戳**算 dt，不能假设每拍新鲜 |
| FOC 在 ADC ISR 内做 SPI 读，SPI 阻塞风险高 | `app_foc.c:293` 调 `app_encoder_sample_rotor()`；`drv_spi.c:33` 仅按循环次数上限，非时间界 | SPI 采样应与电流环分核/分层 |
| 电流环 PI 圆形限幅口径偏大 15.5% | `app_foc_current.c:345`：`v_max=(2D-1)·vbus/1.5`；`foc_modulation.c:56`：`span_max=(2D-1)·vbus`，平衡时 `span=√3·A` → `A_max=(2D-1)·vbus/√3` | **确认缺陷**：应除 `√3`；过大会让 PI 误以为未饱和 → 抗饱和失效 |
| 故障门控把 WARNING 也当 FAULT | `app_foc.c:157`：`state != NORMAL` 即停机 | WARNING 是去抖中间态，不应切 FAULT |
| 主/ISR 竞态：状态与输出所有者不唯一 | `app_foc_run_body` 在主循环改状态/限幅并走 vtest，ISR 也在改输出；`app_foc_current_vtest_step` 在主循环执行 | 需要**单一实时输出所有者** |
| 紧急停桥依赖主循环 | `app_foc_isr_step` 跳闸只走 `app_foc_current_protect()`（零矢量），`app_3phase_inverter_disable()` 在主循环 | 故障停机需独立于主循环 |
| 母线电压可能陈旧 | `app_foc_isr_step` 的 v_bus 来自 1kHz ADC1 慢通道 | 需时效判定与兜底 |

> 说明：**不采信**此前报告中“SPI 超时 = 2 帧 × 3 循环”的结论——`drv_spi.c` 单帧快速路径
> 在 `frames==1` 时直接返回，SDK 重试路径不参与，该推算错误。EMI 导致坏帧的说法亦**未被证实**，
> 本文按“帧无校验、坏值可能到达”做防御性设计，不声称 EMI 为根因。

---

## 2. 架构决策

### 2.1 采样器所有权（单所有者）

```
GPTMR1_CH3 (12.5kHz, PLIC prio 3)  ──ISR──►  app_encoder_sample_rotor_at(now_cycles)
                                                   │  SPI3 读（唯一物理访问点）
                                                   ▼
                                          algo_encoder_snapshot（写者）
                                                   │ seqlock 发布
                                                   ▼
              ┌────────────── 读快照（无 I/O、无自旋）──────────────┐
        25kHz ADC0 ISR (prio 2)                         主循环 / Debug / 辨识
        app_foc_isr_step                                 app_debug_foc / 终端
```

- **GPTMR1 CH3**（全局通道 7）：不占用 ADC1 慢速触发所用的 **GPTMR0 CH2**（全局 2），
  也不占用 `app_gptmr` 现有 CH0/1/2（GPTMR1，全局 4/5/6）。
- 频率 **12500 Hz**：`12.5kHz = 25kHz / 2`；用时间戳而非拍数保证 dt 正确。
- **PLIC 优先级**：`drv_gptmr.c:310` 对 GPTMR 固定 `intc_m_enable_irq_with_priority(irq, 3)`，
  ADC0 在 `drv_adc.c:270` 固定 `2`。数字越大优先级越高 → GPTMR 采样 ISR 可抢占 ADC0 ISR。
  （若将来改为可配置，需在实现中显式断言 `GPTMR prio > ADC0 prio`。）
- **单一所有者**：采样器启动后，`app_encoder_read_raw(APP_ENCODER_ROTOR, ...)` 及
  `app_encoder_read_reg` 运行期**不再触碰物理 SPI**，只返回快照；启动初始化阶段（采样器
  未启动）允许直接 SPI，用于自检/零点标定。

### 2.2 一致快照（seqlock，ISR 侧有界）

- 写者（GPTMR ISR）：`gen++`（变奇）→ 写 payload → `gen++`（变偶）。写侧无阻塞。
- 读者：读取 `gen0`、payload、`gen1`；仅当 `gen0==gen1` 且偶数时接受；否则**有限重试**。
- **高优先级 ISR（ADC0 FOC）绝不无限自旋**：`algo_encoder_snapshot_read_isr()` 最多重试
  1 次，失败即返回“使用上一拍缓存”的 `valid=false`（当拍走保护零矢量）。写者是低优先级
  中断，理论上不会打断读者形成活锁，但仍以有界为准。
- payload 字段：`raw`、`valid`、`seq`（仅成功/接受样本递增）、`timestamp_cycles`、
  `age_cycles`（读取时计算）、`jumped`（本拍被拒）、`consecutive_fail`。

### 2.3 坏帧与失败策略

- 读取 SPI 失败：`consecutive_fail++`；**不发布**新 raw；`valid` 置 false；保留上一有效角
  （`hold`），序号不推进。
- 跳变（坏帧）：`delta` 超限 → 拒绝该 raw（**不写入发布值**，修复 2.1 缺陷），
  `jumped=true`、`jump_count++`、`consecutive_fail` 处理为“跳变连续计数”。
- 连续失败/跳变达到阈值（默认 3 拍）→ 快照 `valid=false` 并置 `fault_latched`，
  FOC 侧走保护零矢量；恢复后自动解除（非锁存 FAULT，除非上层判据）。
- `seq` 只在接受样本时递增 → 消费方以 `seq` 变化判“新样本”，以 `timestamp_cycles` 算 dt。

### 2.4 25kHz FOC 复用 12.5kHz 样本

- `app_foc_isr_step` 每拍读取快照：
  - 若 `seq` 未变（复用）：**不重新 step 角度链**，沿用上拍 `theta_e/omega_e`；
    电流环仍每拍跑（电流是 25kHz 新鲜）。
  - 若 `seq` 变化：用 `timestamp_cycles` 与上一接受样本时间戳之差算 `dt`（不是 FOC 周期），
    `foc_angle.step(theta_m, dt)`。
- 时效判据：`age_cycles ≤ (1/12500 + margin)` 换算；文档化为“最大允许新鲜度”。
  80µs 采样周期下，25kHz FOC（40µs）最多复用 2 拍；超过 margin 判陈旧。
- **每次退出统一计时**：ISR 入口记 `t0`，所有路径（含故障/无效输入/正常）汇合到单一
  `exit` 点计算耗时与预算，避免“早退路径不检查预算”。

### 2.5 单一实时输出所有者与竞态

- **实时输出所有者 = ADC0 ISR**：正常电流环、vtest、calib、zero 的占空比写入都只在
  ADC0 ISR（或由 ISR 消费的目标）中发生。主循环**不再**直接写逆变桥占空比。
- 状态与给定通过**有界临界区**或单字邮箱从主循环交给 ISR；ISR 只读一致快照。
- `app_foc_current_vtest_step` 改为“请求 + ISR 执行”：主循环 `vtest_start()` 只设置请求与
  参数（并在临界区发布），实际调制在 ISR 内按真实 dt 执行。
- 模式：`current / vtest / calib / zero` 由 `s_output_mode` 单一变量决定，ISR 按模式分派；
  主循环只能通过“请求”切换，切换前先 `zero` 并复位相应状态。
- 状态机 `app_foc_disable()`：先输出零矢量/关桥，再复位算法状态，最后 `OFF`（顺序保证
  ISR 不会在复位中途读到半状态）。

### 2.6 故障门控

- `app_foc_run_body`：仅当 `app_fault_get_state() == APP_FAULT_STATE_FAULT`（或锁存位非 0）
  才切 FOC FAULT；`WARNING` 仅记录，不停机。
- **紧急停机独立主循环**：`app_foc_isr_step` 检测到跳闸/母线非法/快照失效且已使能时，
  直接执行 `app_3phase_inverter_emergency_stop()`（关桥 + 强制低 + 关 12V），并置
  FAULT 请求位；主循环只做状态同步。
- 陈旧母线/ADC：v_bus 时效超限 → 保护零矢量；`app_adc_get_raw` 失败 → 保护零矢量。

### 2.7 PI 限幅与饱和

- `v_max = (2·duty_max − 1)·v_bus / √3`（修正 `/1.5`）。
- **移除**“电压饱和 → q 轴给定置零”的 bang-bang 逻辑：它与 PI 抗饱和语义冲突且会造成
  转矩断续。改为：仅保留 `saturated` 标志 + 积分抗饱和；v_scale<阈值只做观测/告警，
  不再硬置零。
- 速度限速滞环保留（独立于电压饱和）。

### 2.8 PWM 三相同步提交（依据 SDK，已查；**高功率阻塞项**）

- 已查安装的 HPM SDK 1.12.1（`hpm_pwm_drv.h`）：
  - 原子提交机制存在：`PWM_SHCR_SHLKEN` + `pwm_shadow_register_lock()` + 写 CMP +
    `pwm_issue_shadow_register_lock_event()`；CMP 的 `update_trigger` 可设为
    `pwm_shadow_register_update_on_shlk`，从而三相同一锁事件生效。
  - 当前驱动 `drv_hrpwm.c` 使用 `pwm_shadow_register_update_on_modify`（逐相 CMP 写后立即生效），
    `hrpwm_write_cmp_pair` 仅 unlock，未使用锁事件。
- **阻塞项（不得淡化）**：三相共 6 次 CMP 写不是原子操作。ADC0 ISR（优先级 2）在写序列期间
  **可被 GPTMR 采样 ISR（优先级 3）抢占**；GPTMR ISR 单拍可达 11µs 以上（含 SPI 读）。
  因此相间提交偏斜可达 **GPTMR ISR 时长量级（≥11µs，非“数条指令”）**，在 25kHz（40µs）下
  可达 25%+ 周期，属**高功率/大电流下必须解决的安全阻塞项**，而非可忽略的抖动。
- **决策（保守，未改）**：无板不可验证锁事件时序，按“不臆造硬件语义”原则**不改**驱动；
  **不做“仅屏蔽 IRQ 覆盖六次写”的实现**（SDK 影子锁时序未上板验证，猜测可能引入更危险行为）。
  上板前禁止带载/大电流运行；验收标准 = 示波器确认三相在同一重载点提交。
- 采样窗口约束（计算并复核）：`duty_max=D` → `span_max=(2D−1)·v_bus`，平衡三相 `span=√3·A`
  → `A_max=(2D−1)·v_bus/√3`。控制层另设保守有效上限 `D_eff=min(user D, 0.70)`（初步，
  为四槽 PMT 时序余量；须示波确认），对 PI/调制/vtest 一致生效。

### 2.9 调试接口（Ozone）

- 新增 `App/Debug/Inc/app_debug_foc.h` + `Src/app_debug_foc.c`。
- `volatile app_debug_foc_t g_app_debug_foc` 置于 `.noncacheable.bss`。
- **命令/请求域与状态域分离**：
  - 请求域（主循环写、ISR 读）：`enable_req`、`disable_req`、`clear_fault_req`、
    `cal_encoder_req`、`vtest_req`、`param...`，每项 `ack` + `result`（一次性命令）。
  - 状态域（ISR/主循环写、调试器只读）：`state`、`fault`、`ready`、`isr_cycles`、
    `isr_cycles_max`、`enc_age_us`、`enc_seq`、`enc_errors`、`enc_jumps`、
    `i_d/i_q`、`i_d_ref/i_q_ref`、`duty[3]`、`v_bus`、`v_scale`、`saturated`。
- **无 Control→Debug 依赖**：`app_debug_foc.c` 依赖 `App/Control` 与 `Interface`，
  反向不成立。
- 主循环命令处理**有界**（每拍处理固定数量请求），不阻塞。
- 紧急停机经 Interface/Control 机制，由 ISR 轮询请求，不依赖调试层。

### 2.10 编码器自检：被动健康 vs 通电电气标定

- **被动健康（passive）**：不使能桥，读 RD 寄存器、读角度、跳变/错误计数、SPI 速率与耗时；
  明确输出 `passive_ok`。
- **通电电气标定（energized）**：在零给定/受控小电压下核对电流符号、相序、电角方向与
  编码器偏移；输出 `energized_ok`、`elec_offset_rad`、`direction`、`max_current_a`、
  `duration_ms`。二者字段分开，互不覆盖；未做则 `energized_ok=false, not_run`。

### 2.11 仅调试构建的台架模式

- 编译期开关（如 `APP_DEBUG_BENCH_ONLY=1`）在 `app_init` 中选择性跳过 USB/终端初始化，
  保留 RTT + 最小启动，便于台架用 Ozone 直控。
- **默认安全启动**：`OFF`、零给定、不自动使能、不自动通电。
- **保留用户配置**：不改 `config/*.yaml`；用户的脏 `software.yaml`（kp=0.3、ki=100、
  i_trip=0、speed=0）原样保留。调试模式若需安全包络，用**运行期显式请求**（一次性命令）
  而非改默认值。

---

## 3. 安全边界与风险

1. **采样器优先级**：GPTMR(3) > ADC0(2)。若将来有人把两者优先级对调，seqlock 读者可能被
   写者打断 → 但读者有界重试，不会死锁。文档强调不得对调。
2. **坏帧无校验**：KTH7823 帧无 CRC，跳变阈值是启发式；高加速工况可能误判。阈值
   （5°机械）与连续失败阈值需在台架根据实际转速复核。
3. **复用样本的 ωe**：12.5kHz 角速度在两次 FOC 之间保持；速度环（未在 v1）若引入需注意
   90µs 更新率。
4. **PWM 原子提交**：若 SDK 不支持，三相写序列在 25kHz 下窗口为百 ns 级，风险低但非零；
   实施时以 SDK 事实为准。
5. **调试模式跳过 USB**：会失去终端恢复路径，仅限台架；默认不启用。
6. **不声称硬件成功**：所有结论限于宿主测试 + 编译 + 代码审查。

---

## 4. 设计表（决策速查）

| 主题 | 决策 | 关键文件 |
| :--- | :--- | :--- |
| 采样节拍 | GPTMR1 CH3 @12.5kHz | `app_gptmr.c` / `app_encoder.c` |
| PLIC | GPTMR=3 > ADC0=2（沿用驱动默认） | `drv_gptmr.c` / `drv_adc.c` |
| 单一 SPI3 所有者 | 采样器 ISR；运行期读 API 走快照 | `app_encoder.c` |
| 快照一致性 | seqlock，ISR 读有界重试 | `algo_encoder_snapshot.c` |
| 坏帧 | 拒绝值不发布；连续阈值→invalid | `algo_encoder_snapshot.c` |
| 25kHz 复用 | seq 判新；timestamp 算 dt | `app_foc.c` / `foc_angle.c` |
| ISR 计时 | 单一 exit 统一预算检查 | `app_foc.c` |
| 输出所有者 | ADC0 ISR 独占；主循环只发请求 | `app_foc.c` / `app_foc_current.c` |
| 故障门控 | 仅 FAULT 停机；ISR 紧急关桥 | `app_foc.c` |
| PI 限幅 | `v_max=(2D-1)·vbus/√3` | `app_foc_current.c` |
| 饱和 | 去掉 bang-bang 置零，仅抗饱和+告警 | `app_foc.c` |
| PWM 提交 | 以 SDK shadow/update 事实为准 | `app_hrpwm.c` / `drv_hrpwm.c` |
| 调试 | `g_app_debug_foc`（noncacheable） | `app_debug_foc.c` |
| 自检 | passive / energized 字段分离 | `app_debug_encoder.c` |
| 台架模式 | 编译期旁路 USB/终端，默认 OFF | `app_logic.c` |

---

## 5. 未决 / 未验证（诚实清单）

- **目标板行为未验证**：本设计的所有实时性/PLIC/SPI 结论未上板复测；源码可编译、宿主测试可跑，
  但“上电成功”不作声明。
- **PWM 三相同步提交**：待查 SDK 头文件确认；若不存在原子机制则文档化残余风险。
- **跳变阈值**：需按实际最高转速与机械共振台架复核。
- **供电纪律**：本板须外部供电；调试器供电会导致欠压复位循环（见 AGENTS.md），
  台架验证前先确认。
- **用户配置未改**：脏 `config/software.yaml` 原样保留；`i_trip_a=0` 表示快速过流关闭，
  调试期需在安全包络内手动设置或经调试命令显式写入。
