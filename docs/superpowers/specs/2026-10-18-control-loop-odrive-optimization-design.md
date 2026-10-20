# 控制环 ODrive 参考优化方向（估计器优先）

- 日期：2026-10-18
- 基线：`dd15b50`（30kHz 电流环 + 15kHz 速度环 + 10kHz 位置环已提交）
- 相关设计：`2026-10-16-position-loop-design.md`、`2026-10-15-friction-feedforward-design.md`、`2026-10-11-current-pi-odrive.md`、`2026-09-22-encoder-sampler-foc-realtime-design.md`
- ODrive 对照：v3.x 开源固件 `master`（`Firmware/MotorControl`，NRND）；ODrive 0.6.12 文档；ODrive Pro 规格（24kHz PWM、8kHz 控制环，24kHz 电流环仍为 coming soon；Pro/S1 固件闭源，不能做代码级对照）

## 1. 结论

- 当前架构在结构上已经接近 ODrive：位置 P → 速度 PI → 电流 PI，电流环用 R/L 极点对消，位置输入用二阶临界阻尼滤波。
- 下一步的瓶颈不在“再提采样率”，而在 **估计器**；但估计器不是“一个 PLL 全包”：
  - **FOC 电角度观测器（首要）**：保证 Park 用的 θe 精准、可靠；30kHz；θe 的准确性优先于速度环的平滑。
  - **速度观测器（独立）**：可以独立于电角度观测器；服务速度环；不与 FOC 观测器共享状态/带宽。
- 方向顺序：**P0-A FOC 电角度观测器 → P0-B 独立速度观测器 → P1 位置滤波加速度/惯量前馈 → P1 限幅/保护 → P2 输入轨迹/电流环细节 → P3 可选**。
- P0-B 速度观测器同时输出 ωe 和轴端 speed；FOC 不再单独做 ωe 观测器；FOC 角度链只保留 raw θe + 延迟/插值。合并方案已定，观测器速率/带宽待定。
- 保留当前 30k/15k/10k、三相互感采样、8MHz 编码器、50:1 减速器、单圈位置和请求/执行契约；不移植 ODrive 的 C++ 架构，不照抄 ODrive 的数值/单位。

## 2. 目标 / 非目标 / 反目标

### 2.1 目标

- **P0-A FOC 电角度观测器（首要）**：以转子绝对角为输入，输出 θe（Park/反 Park）；ωe 由 P0-B 提供（合并方案），P0-A 不再单独维护 ωe（可保留故障备份）；首要目标是 θe 的精度/可靠性，不是速度环带宽；编码器坏帧、跳变、方向/零点、强制角/`vtest`/辨识、保持/恢复都要有明确降级，绝不让 Park 使用错误角。
- **P0-B 速度观测器**：与电角度观测器状态/带宽独立，可共享原始转子样本；输出轴端 speed 供速度环，同时输出 ωe 给 FOC（合并方案）；位置环仍使用完整游标 `shaft_rad`；低速纹波和相位滞后相对现状改善。
- **P1 位置前馈**：给位置二阶滤波增加加速度输出，并按 `inertia_eff` 做转矩前馈；`inertia_eff` 默认 0（不启用），提供 YAML 手动填入与后续自整定两条路径。
- **P1 限幅/保护对齐**：按 ODrive 的分层思路补齐 `vel_integrator_limit`、`vel_limit`/overspeed、`spinout`、`current_lim_margin`/热限流等；在 M1 保护动作关闭的前提下先做“检测/告警/可观测”，再逐项决定是否动作。
- **P2 输入轨迹**：评估 `VEL_RAMP`/`TORQUE_RAMP`/`TRAP_TRAJ`，让位置/速度命令的加速度有界。
- **P2 电流环细节**：R·I 前馈、Park/反 Park 相位补偿、调制裕量/高占空比采样复核；不改电流环基本结构。
- **P3 可选**：anticogging、gain scheduling；仅在低速位置纹波/大误差场景确有收益时做。

### 2.2 非目标（本期不做）

- 不改 30k/15k/10k 节拍，不把电流环降到 ODrive 的 8kHz，不照搬 1000 rad/s 电流带宽。
- 不改硬件：不增加编码器/采样通道，不改三相低侧采样，不拆减速器，不改 8MHz SPI。
- 不做输出多圈绝对位置（49:50 + 50:1 硬件只能解析 1 个输出圈）；多圈是单独硬件项目。
- 不做弱磁、MTPA、无感、ACIM；不做 ODrive C++ 架构重写。
- 不把 FOC 电角度观测器与速度观测器强行共用一个状态/带宽；速度观测器可以独立。
- 不降低 15kHz 控制频率来保持旧 hold/fallback 拍数；hold/fallback 按 15kHz 周期派生。
- 不把 100rpm 输出上限改成 60rpm；100rpm 是确认过的有意上限。
- 不在本方向文档里打开 `APP_PROTECT_ACTION_EN` 或设置非零 `i_trip_a`；保护动作需单独硬件/台架评审。
- 不在 P0 内同时改电流环带宽/调制限幅，避免无法归因。

### 2.3 反目标（技术上“做了但更差”）

- FOC 电角度为了平滑牺牲 θe 精度/可靠性；Park 用了带相位滞后的滤波角。
- 两个观测器共享状态/带宽，速度环和电流环互相拖累。
- PLL 把编码器坏帧、齿隙/回差或减速器扭振“平滑”进速度环/电角度，掩盖真实故障。
- 用降控制频率来保持旧 hold/fallback 拍数。
- 直接复制 ODrive 的 `pos_gain=20`、`vel_gain=1/6`、`current_lim=10` 等数值；单位、减速比、电机都不同。
- 一次改多个环，阶跃/纹波变好但无法判断是哪一项贡献。

## 3. 约束与基线

### 3.1 硬约束

- 电流环 30kHz（ADC0 PMT ISR）、速度环 15kHz（GPTMR CH1）、位置环 10kHz（CH0）；不新增 SPI/ADC 读点。
- 术语：`30kHz` = 电流环执行/采样率（ADC0 PMT ISR，33.3µs）；`2kHz` = 电流环目标闭环带宽（`bandwidth_rad_s=12566.370614`，`cal rl`/PI 设计值）；两者不同，文档不写“2kHz 电流环”。
- 三相电流采样保留；编码器 SPI ≤8MHz；`duty_max=0.95`；触发延时 500ns。
- 电机额定 3300rpm；50:1 减速器对应输出约 66rpm。**100rpm 输出是 Kaiser 确认的有意上限**（对应 5000rpm 电机、1.5× 额定、10 极对约 833Hz 电频率）；按计划的短时/受控测试执行，不改为 60rpm。
- 单圈位置：输出角 = 电机多圈 / 50；`algo_motion_vernier` 只用输出游标解 50 圈圈号。
- 速度/力矩模式可以只用 rotor 编码器；位置模式必须完整解算 rotor+vernier 并保证位置精度。
- 热路径 `ATTR_RAMFUNC`、无 Flash 调用、无 RTOS/动态分配/打印；C17；参数走 YAML + flash 记忆。
- FOC 电流源所有权：`DIRECT`/`MOTION`，epoch/sequence，500µs 过期；速度环通过 `app_foc_submit_current` 提交，`app_foc_get_execution` 反馈。
- M1 保护策略：`APP_PROTECT_ACTION_EN=0`、`i_trip_a=0`；保护动作未开。
- 观测器参数放 `config/software.yaml` 的 software 域；flash 记忆迁移不得静默清空已保存的 motion/pos/friction 整定。

### 3.2 当前基线

| 项 | 当前值 | 位置 |
|---|---|---|
| 电流环 | 30kHz 执行/采样；目标带宽 2kHz（`bw=12566.370614 rad/s`）；`kp=1.489114918`、`ki=1985.486557`；`decoupling=0` | `config/software.yaml`、`app_foc_current.c`、`foc_current.c` |
| 速度环 | 15kHz；`kp=50 Nm/(rad/s)`、`ki=1000 Nm/rad`；摩擦 1.1/-0.3/0.05；`velocity_lpf_coeff=0.05` | `app_motion.c`、`algo_motion.c` |
| 位置环 | 10kHz；`pos_gain=50`、`input_filter=20 rad/s`；二阶滤波 + 速度前馈 | `app_motion_position_step`、`algo_motion.c` |
| FOC 角度 | 30kHz；θe 原始转子角 + 极对数/方向/零点；ωe 差分 + 100Hz LPF | `foc_angle.c`、`app_foc.c` |
| 限幅 | `i_q_max=24.3A`、`motion_iq_max=25A`、`i_trip=0`、`duty_max=0.95`、`speed_max=0`、`motion_speed_limit=0` | `config/software.yaml` |
| 保护 | `APP_PROTECT_ACTION_EN=0` | `app_protect_policy.h` |
| 编码器 | 转子 1:1 16bit + 输出 49:50 16bit；8MHz；50 电机圈 = 1 输出圈 | `app_encoder.c`、`app_motion.c` |
| 位置语义 | 单圈 [0,2π)；误差取模 ±π；位置模式要求完整 pair | `algo_motion.c`、`app_motion.c` |
| 标定 | `cal encoder`、`cal rl`、`cal motion`、摩擦辨识、模拟零偏 | `App/Control/Identify/*`、`app_analog_signal.c` |

**速度/角度口径（不要混用）**

| 量 | 单位 | 来源 | 消费者 |
|---|---|---|---|
| `ωe` | 电角 rad/s | speed observer（合并方案）；`foc_angle` 仅故障备份 | FOC 解耦、FOC `speed_max` 过速保护、遥测；**不是速度环反馈** |
| `s_motion.speed_rad_s` | 轴端机械 rad/s | `app_motion_step`（转子差分/减速比 + LPF） | 速度 PI、摩擦前馈、位置级联 |
| `s_vel_cmd_rad_s` / `s_target` | 轴端机械 rad/s | 位置环输出/速度命令 | 速度 PI 目标 |
| `speed_max_rad_s` | 电角 rad/s | `config/software.yaml` | FOC 过速保护（当前 0=关） |
| `motion_speed_limit_rad_s` | 轴端机械 rad/s | `config/software.yaml` | 速度/位置命令限幅（当前 0=不限） |
| `shaft_rad` | 轴端 rad | `algo_motion_vernier` | 位置环反馈 |

- 关系：`ωe = pole_pairs × direction × reducer × ω_out`。10 极对、50:1、dir=+1 时，1 rad/s 轴端 = 500 rad/s 电角；100rpm 轴端 = 10.47 rad/s = 5236 rad/s ≈ 833Hz。
- 当前 `decoupling=0`、`speed_max=0`，所以 `ωe` 目前主要是遥测/保护预留；速度环实际用的是 `s_motion.speed_rad_s`。P0-A 的 ωe 与 P0-B 的 speed 是两条独立链。
- `ωe` 可换算为轴端速度：`ω_out = ωe / (pole_pairs × direction × reducer)`，技术上可作速度源；但当前 30kHz 差分 + 100Hz LPF，低速量化约 0.0575 rad/s 轴端，并与 FOC 角度/保持语义耦合。**P0-B 独立速度观测器仍是速度环主反馈；`ωe` 只作交叉检查/降级备份**。

### 3.3 ODrive 对照摘要

| 项 | ODrive v3.x | 当前 | 影响 |
|---|---|---|---|
| 节拍 | PWM 24kHz；控制/电流环 8kHz 同频 | 电流 30k、速度 15k、位置 10k | 当前电流环更快；多速率需处理 15k/30k 交接 |
| 电流环 | PI 极点对消，默认 `bw=1000 rad/s`；0.80 调制限幅；`R_wL`/`bEMF` 前馈可选；`current_lim+margin`；热限流；相位时间戳；SVM | 2kHz；`duty_max=0.95`；解耦可选；`i_q_max/motion_iq_max`；`i_trip=0`；同角；min-max | 当前更激进、保护更弱；ODrive 更保守、前馈/保护更完整 |
| 电角度/速度 | 单编码器 PLL：`pos_estimate`/`vel_estimate`；`phase` 由插值编码器 + offset，`phase_vel` 由 PLL 速度；默认 `bw=1000 rad/s` | `foc_angle` 30k 原始 θe + 100Hz ωe；`app_motion` 15k 差分 + LPF 0.05 | 目标拆分：FOC 电角度 primary + 独立速度观测器；不照搬单 PLL 共享 |
| 速度环 | `vel_gain=1/6`、`vel_integrator_gain=2/6`、`vel_integrator_limit=INF`、`vel_limit=2`、tolerance 1.2、overspeed、ramp | `kp=50`、`ki=1000`、积分限幅=转矩限幅、无独立积分限幅/overspeed/ramp；有摩擦前馈 | 结构同 PI；ODrive 限幅/保护更完整 |
| 位置环 | P 20 + `POS_FILTER`（`accel×inertia` 转矩前馈）+ gain scheduling + trap/ramp + circular 可选 + 多圈 `pos_estimate` | P 50 + 二阶滤波 + 速度前馈 + 外部 `torque_ff`；无 accel/惯量前馈；单圈 | ODrive 前馈/轨迹更完整；当前少一条 accel 前馈 |
| 位置反馈 | 编码器 PLL；`pos_estimate` 多圈；`load_encoder` 可选 | 电机/50；游标只解圈号；单圈；速度/力矩可用 rotor-only，位置要完整 vernier | 位置模式完整解算；单圈是硬件限制 |
| 限幅/保护 | `current_lim+margin`、热、`torque_lim`、`vel_limit/overspeed`、`spinout`、DC 母线/制动 | M1 只检测；`i_trip=0`；限速 0 | ODrive 量产保护；当前台架 |
| 输入 | passthrough/vel ramp/torque ramp/pos filter/trap/mirror/tuning | 位置二阶滤波 + 直接命令 | ODrive 输入整形更丰富 |
| 标定 | R/L→PI、编码器 offset、DC 偏置、anticogging | `cal encoder`/`cal rl`/摩擦/游标/零偏 | 当前电流/摩擦自动整定更完整；ODrive 有 anticogging |
| 代码/单位 | C++ OO、InputPort/OutputPort、单节拍；turn/Nm | C 分层、请求/执行邮箱、多速率；rad/Nm 轴端 | 当前更解耦；增益数值不可直接对照 |

## 4. 优化方向

### 4.1 P0 估计器（本次讨论主项）

#### 4.1.1 现状与证据

- FOC 电角度链：`foc_angle_step()` 30kHz；θe 由原始转子机械角、极对数、方向、零点得到；ωe 由 θe 差分再经 100Hz LPF。θe 的准确/可靠是 FOC 首要；ωe 的平滑/带宽是次要。
- 运动速度：`app_motion_step()` 15kHz；速度 = `wrap(rotor.rad - prev) × direction / sample_dt / reducer`，再过 `velocity_lpf_coeff=0.05`（15kHz 约 120Hz）。
- 结果：同一转子角有两套估计（30kHz 电角度/ωe、15kHz 轴端 speed），带宽/更新率/相位不同；低速一阶差分有量化台阶，LPF 有相位滞后；100rpm 输出对应约 833Hz 电频率，100Hz 的 ωe LPF 在该频段会衰减/滞后。

#### 4.1.2 ODrive 参考

- `Encoder::update()` 用二阶 PLL：`pll_kp=2·bw`、`pll_ki=0.25·pll_kp²`；输出 `pos_estimate`、`vel_estimate`；默认 `bandwidth=1000 rad/s`；带 `snap_to_zero_vel` 和相位插值。
- 关键：ODrive 的 FOC `phase` 由 `interpolated_enc + phase_offset` 得到，`phase_vel` 由 PLL 速度得到；PLL 的 `pos_estimate` 主要供位置/速度，不是把 PLL 位置滤波直接塞进 Park。
- 本工程按用户定案拆成 **FOC 电角度观测器（θe 精准可靠优先）+ 独立速度观测器**；ODrive 的单 PLL 只作结构参考，不强行共享状态/带宽。

#### 4.1.3 方向（用户定案）

##### P0-A FOC 电角度观测器（首要）

- 位置：30kHz ADC ISR / `foc_angle` 路径；输入为转子绝对角、方向、极对数、电角度零点；输出 θe（Park/反 Park）与 ωe（解耦/限速）。
- 首要目标：θe 精准、可靠；不为速度环的平滑牺牲角度；任何降级不得把错误角送入 Park。
- **当前电角度链的具体不足（P0-A 依据）**：
  1. **无输出/相位延迟补偿**：Park/反 Park 用同一 θe；duty 在 ISR 后写入，作用于下一 PWM 周期；100rpm/833Hz 下约 10–15° 电角误差。ODrive 用 `i_timestamp`/`output_timestamp` 补偿；当前 `app_foc_current_run_fresh()` 的 `foc_sincos()` 只算一次 `s/c`，Park 与反 Park 共用。
  2. **无采样间插值**：16bit 绝对角在 30kHz 更新；833Hz 下每拍约 10° 电角，角度零阶保持；ODrive 用 `interpolation_`。16bit 量化 0.055° 不是主要问题，采样保持/延迟才是。
  3. **ωe 一阶差分 + 100Hz LPF**：833Hz 下衰减/滞后；解耦默认关、FOC 限速 0，短期影响小；启用解耦/限速前必须提高带宽/换观测器。
  4. **保持/故障语义**：坏帧复用 `s_theta_held`，`APP_PROTECT_ACTION_EN=0`；32 拍≈1.07ms（833Hz 约 0.9 电周期）后才计数；高转速下保持角误差风险，需要更短/更明确的降级。
  5. **静态零点**：`cal encoder` 残差 2.71° 电角；直接进入 θe；不是观测器能修的；需要 `cal`/补偿/验收。
  6. **没有模型/状态观测**：角度直接来自 raw+offset；没有预测/噪声模型；可靠性依赖编码器 jump/age/seq；P0-A 可在不改 raw θe 的前提下加 ωe 观测/延迟补偿。
- 结构候选（下一轮定）：
  1. **θe 走原始转子角 + offset；观测器只估 ωe**（Mephisto 优先；不把 PLL 位置滤波塞进 Park；100rpm/833Hz 最稳）。
  2. PLL/跟踪观测器同时估 θe/ωe；平滑但引入位置相位滞后，必须单独验证 833Hz 下的角度误差/电流稳定性。
  3. **角度插值/输出延迟补偿 + ωe 观测器**（参考 ODrive 的插值/时间戳；当前不足 #1/#2 的直接修复；建议与候选 1 组合，而不是单独 PLL）。
- 可靠性：
  - 编码器坏帧/超龄/跳变、方向/零点、`cal encoder` 结果；
  - 强制角/`vtest`/辨识路径继续使用现有角度链，不被观测器影响；
  - 保持/恢复、`APP_FOC_ENC_DEG_TRIP_STREAK`、`s_theta_held` 的语义；
  - 100rpm 输出（5000rpm 电机、10 极对、约 833Hz 电频率）是角度观测器的高频上限。
- 合并后 P0-A 不再维护 ωe；`foc_angle` 的 100Hz LPF 仅作故障备份/待移除；解耦/FOC 限速改用 P0-B 的 ωe，833Hz 下不能假装 100Hz 足够。

##### P0-B 独立速度观测器

- 位置：建议 15kHz 速度拍（或 30kHz 但独立状态）；输入转子绝对角；输出轴端 speed 供速度环；位置环仍用完整游标 `shaft_rad`。
- 与 P0-A 独立：可共享原始转子样本，但不共享状态/带宽；速度环的平滑/带宽不为 FOC 角度让路，反之亦然。
- 结构候选：二阶 PLL/跟踪观测器；M/T/自适应窗口作为退路；`velocity_lpf_coeff` 移除/保留作为降级路径，下一轮定。
- 速度/力矩模式：只用 rotor 编码器；不再因为游标坏帧把整个速度/力矩环拖入零力矩 hold。
- 位置模式：必须完整解算 rotor+vernier，`algo_motion_vernier` + `shaft_rad` 完整精度；`app_motion_step` 按 mode 分支，位置模式保留完整 pair/精度校验。

##### 合并方案（用户定案）

- 如果 P0-B 速度观测器同时输出 ωe 和轴端 speed，则 FOC 不再需要单独的 ωe 观测器；P0-A 只保留 raw θe + offset + 延迟/插值/可靠性。
- 角色：P0-A = FOC 角度（θe）；P0-B = 速度/ωe（speed 给速度环，ωe 给 FOC 解耦/限速）；共享原始样本，但 P0-A 不做速度滤波、P0-B 不做 Park 角度。
- 前提：P0-B 必须是 speed-quality（1rpm、100rpm、带宽/延迟），不能只是 FOC 角度的副产品；更新率 30k/15k 需定（FOC ωe 30k、速度 15k 可折中）。
- 收益：一套速度状态、FOC/速度环同源 ωe；代价：速度观测器故障同时影响 FOC 解耦/限速与速度环；FOC 角度 raw 仍独立。

##### ωe 在 FOC 中的消费者（当前代码）

1. **解耦前馈**：`foc_current_step()`；`decoupling_en=0` 默认关；`vd -= ωe·Lq·iq`，`vq += ωe·(Ld·id + λ)`。
2. **FOC 过速保护**：`app_foc_run_body()` 的 `speed_max_rad_s`（电角）；`s_speed_limited` → `s_i_q_ref=0`；当前 `speed_max=0` 关。
3. **遥测**：`g_foc_current_snapshot.omega_e_rad_s`、`foc show`/Ozone。
4. **`foc_angle` 内部**：ωe 是输出，不参与 θe；坏帧时 `s_omega_held` 保持。

- 速度环不消费 `ωe`；它用 `s_motion.speed_rad_s`。
- 合并方案：P0-B 输出 `ωe + speed`；FOC 的 1/2/3 用 P0-B 的 `ωe`；速度环用 P0-B 的 `speed`；P0-A 只保留 raw θe + 延迟/插值。

##### 参数与调试

- 观测器参数：`config/software.yaml` 增加 `control.foc_observer` / `control.speed_observer`（字段/默认值在 plan 定；例如 `omega_bandwidth_rad_s`、`bandwidth_rad_s`、`enable`、`angle_mode`）。
- flash 迁移：扩展现有 software 记录会因长度/代次不匹配整条拒绝（`app_param_record_load`）；必须版本化迁移，不能静默清空已保存的 motion/pos/friction 整定。
- 低速纹波：提供调试全局变量（`.noncacheable.bss`，Ozone/RTT 直读，仅调试）：如 `g_motion_speed_dbg_rad_s`、`g_motion_speed_ripple_pp_dbg`、`g_motion_speed_ripple_max_dbg`；在 1kHz 窗口/速度拍更新；不进入正式控制，不新增长捕获/命令。
- hold/fallback：按 15kHz 周期从时间派生，保持 15kHz 控制频率；20ms/500ms → 300/7500 拍（`APP_MOTION_HOLD_MAX_SAMPLES` / `APP_MOTION_VERNIER_FALLBACK_MAX_SAMPLES`）。

#### 4.1.4 验收

- 宿主：
  - P0-A：θe 斜坡/阶跃、0/2π wrap、方向 ±、零点、833Hz 等效输入、首拍、反转、编码器坏帧/超龄/跳变、强制角/`vtest`/辨识路径、NaN、角度误差有界。
  - P0-B：速度阶跃/斜坡、wrap、方向、减速比、首拍、反转、丢样/保持、量化、带宽钳位；速度误差有界。
- 台架：
  - FOC 电角度：100rpm 输出（5000rpm 电机、约 833Hz 电频率）下 θe 误差/电流稳定性；对比“无补偿 / 仅 ωe 观测 / ωe + 延迟补偿 + 插值”；`cal encoder`/`cal rl` 仍通过；`foc trace` 电流纹波不增大。
  - 速度：低速 1/10/100rpm（输出端）速度纹波相对现状改善；速度阶跃超调/建立时间不劣化；调试全局变量记录。
  - 位置：完整 vernier 解算；位置跟随误差/回差/保持精度不劣化；位置模式不使用 rotor-only 降级。
- 实时：30kHz ISR 耗时/预算、15kHz 入口抖动；热路径无 Flash；全量测试/构建无回归。
- 具体阈值在第一轮台架后定；首轮以“θe 不劣化、速度相对改善且不引入新故障”为门禁。

#### 4.1.5 风险

- P0-A：PLL 位置滤波在 833Hz 下相位滞后/角度误差；原始角量化不是主因，输出延迟/零阶保持约 10–15° 电角需候选 3；编码器坏帧/跳变；方向/零点/标定；强制角/辨识路径误用观测器；30kHz ISR 负载。
- P0-B：带宽过高噪声/过低滞后；15k/30k 交接；反转/齿隙/扭振进入速度；游标坏帧对速度/力矩的影响（rotor-only 后应消除，但需验证）。
- 两个观测器：电角度 speed limit 与速度环 speed 可能不一致；职责边界/调试口径要明确。
- 位置模式：完整 vernier 的精度/回差/保持；rotor-only 只用于速度/力矩，不能误入位置。
- hold/fallback：300/7500 与 15k 的兼容；恢复/反转。
- 参数：software.yaml 扩展/flash 迁移；不能静默丢 motion/pos/friction。

#### 4.1.6 待讨论问题（下一轮）

1. P0-A 结构：候选 1（原始 θe + ωe 观测器）还是候选 2（PLL 同时估 θe/ωe）？是否叠加候选 3（输出延迟/插值补偿）？833Hz 下 θe 误差/相位/电流稳定性如何验收？
2. 合并后 ωe 由 P0-B 提供：P0-B 的 ωe 带宽/更新率；`foc_angle` 100Hz LPF 仅备份/移除；解耦/FOC 限速用 P0-B 的 ωe。
3. P0-B：15kHz 还是 30kHz 独立状态；PLL/跟踪结构；带宽；`velocity_lpf_coeff` 移除/保留；与 P0-A 的独立性边界。
4. 编码器坏帧/超龄/跳变、方向/零点、强制角/`vtest`/辨识、保持/恢复在 P0-A/P0-B 各自的降级语义。
5. 位置模式完整 vernier 的精度/回差/保持；`app_motion_step` 的 mode 分支；速度/力矩 rotor-only 的 `feedback_valid`。
6. `software.yaml` 的 `control.foc_observer` / `control.speed_observer` 字段/默认值；flash 迁移/版本；不静默丢 motion 调参。
7. 测试：100rpm/833Hz 的 FOC 角度验收；低速全局变量；是否先做算法 spike。

### 4.2 P1 位置滤波：加速度/惯量前馈

- ODrive `POS_FILTER`：`accel = kp·Δpos + ki·Δvel`；`torque_setpoint = accel × inertia`；当前 `algo_motion_pos_filter_step()` 已算 `accel` 但只更新 `pos/vel`，没有输出。
- 方向：给 `algo_motion_pos_filter_t` 增加 `accel` 输出；位置拍把 `accel × inertia_eff` 加到速度 PI 输出（或独立前馈项）。`inertia_eff` 为轴端等效惯量（电机惯量 × 减速比² + 负载惯量），不能直接用电机的 `inertia_kgm2`。
- 默认/来源：`inertia_eff` 默认 0（不启用前馈）；`config/software.yaml` 的 `control.position_loop.inertia_eff` 手动填入；后续自整定写同一参数（两条路径）；`inertia_kgm2=2.3e-5` 是电机转子惯量，经 50:1 反射约 0.0575 kg·m²，未含负载，不能直接当 `inertia_eff`。`input_filter_bandwidth=0` 时滤波直通、不计算 accel，FF 自然为 0；P1 明确“FF 仅在 bw>0 时有效”，或强制 bw>0。
- 验收：位置阶跃/跟随误差改善、无超调、电流不饱和；速度环/限幅不回归；`inertia_eff=0` 时行为与现状一致。
- 风险：惯量估计误差、减速器/负载变化、加速度噪声、滤波带宽与转矩限幅。

### 4.3 P1 限幅/保护对齐

- ODrive 分层：`current_lim + current_lim_margin`、热限流、`torque_lim`、`vel_limit/vel_limit_tolerance/overspeed`、`vel_integrator_limit`、`spinout`。
- 当前：`i_q_max=24.3`、`motion_iq_max=25`、`i_trip=0`、`APP_PROTECT_ACTION_EN=0`、`speed_max=0`、`motion_speed_limit=0`。
- 方向：
  - 先对齐 `motion_iq_max` 与 `i_q_max`，减少满转矩时 `current_limited` 的假象；
  - 速度 PI 增加独立 `vel_integrator_limit`（当前积分限幅=转矩限幅）；
  - 评估 `vel_limit`/tolerance/overspeed 的检测与告警；
  - `spinout`（机械/电功率判据）先做观测，不直接停机；
  - 热限流/`current_lim_margin` 需硬件温度/电流能力确认，单列。
- 反馈语义：速度/力矩只用 rotor；位置模式完整 rotor+vernier；`feedback_valid`/hold 按 mode 区分；位置模式不得用 rotor-only 降级。
- 约束：M1 动作关闭；任何停机类动作必须单独评审，不在本方向默认开启。
- 验收：故障注入/限幅场景不误报；`current_limited` 语义清晰；M1 行为不变；位置模式精度不降。

### 4.4 P2 输入轨迹

- ODrive：`VEL_RAMP`、`TORQUE_RAMP`、`TRAP_TRAJ`；当前只有位置二阶滤波 + 直接速度/力矩命令。
- 方向：先评估位置命令的梯形/加加速度限制，再评估速度/力矩斜坡；不改变现有位置滤波 API 的语义，新增模式或参数。
- 验收：大行程阶跃的加速度/电流有界；位置超调/跟随误差改善；现有命令兼容。

### 4.5 P2 电流环细节

- ODrive：`R_wL_FF`（含 R·I）、`bEMF_FF`、0.80 调制限幅、`current_lim_margin`、Park/反 Park 时间戳相位补偿。
- 当前：解耦只有 ωL·I + ωλ，无 R·I；`duty_max=0.95`（30k/sample15 采样有效上限约 0.84）；Park/反 Park 同角；`cal rl` step 纹波 0.115–0.144A 未过 0.10A。
- 方向：补 R·I 前馈；评估 PWM 输出相对采样的相位补偿；复核 `duty_max` 与高占空比采样；电流环带宽/`cal rl` 门限不在 P0 改。
- 验收：`cal rl` step/纹波、`foc trace`、高占空比电流波形、ISR 负载；不回归。

### 4.6 P3 可选

- anticogging：50:1 会衰减电机 cogging 对输出端的影响；仅在低速位置纹波实测需要时做。
- gain scheduling：大位置误差下的 V 形增益调度；当前单圈、P=50，先评估是否需要。
- 两者都不进入 P0/P1 的关键路径。

### 4.7 P0 基线卫生（先做，避免污染估计器结论）

- `app_motion.h`：`app_motion_step` 注释 10kHz（实际 15kHz）；位置 5kHz（实际 10kHz）；`motion_iq_max` 注释 1A（实际 25A）；“可多圈”（实际单圈）；`APP_MOTION_CMD_TARGET_LIMIT` 的“50Kt”旧口径。
- `APP_MOTION_HOLD_MAX_SAMPLES`：按 15kHz 派生，20ms → 300 拍；`APP_MOTION_VERNIER_FALLBACK_MAX_SAMPLES`：500ms → 7500 拍；保持 15kHz，不降频。
- `app_foc.h`/`app_foc_current.h`/`app_encoder.h`/`app_analog_signal.h`/`app_motion_timer.h`/`app_logic.c` 的 25k/5k 注释；`APP_FOC_ENC_PERIOD_US=40`、`APP_FOC_ISR_BUDGET_US=25`、`ENC_DEG_TRIP_STREAK` 的 1.3ms 注释、`APP_FOC_CURRENT_AVG_ALPHA` 的 32Hz 注释、`APP_FOC_TRACE_MAX` 的 5.1ms 注释。
- `app_encoder_output_start()` 的 40µs 年龄上限仍按 25kHz 算；`g_encoder_sample_count` 注释 25000/s；实际 30k。
- `motion_iq_max=25` vs `i_q_max=24.3`：对齐或明确“motion 允许超 0.7A 由 FOC 限幅”的语义。
- `app_analog_signal_process()` 实际 1kHz，但滤波器 `sample_rate_hz=pwm_freq=30k`；当前电流/母线滤波器都是 NONE，若启用 LPF 会错。
- 说明：hold/fallback、iq 上限对齐是行为变更，不是纯注释；先定语义再改。

## 5. 实施顺序与门禁

1. **P0 卫生**：注释/常量/语义对齐；hold/fallback 按 15kHz 派生 300/7500；`motion_iq_max`/`i_q_max` 对齐；观测器参数 software.yaml 的 schema/迁移。
2. **P0-A FOC 电角度观测器（首要）**：先定候选 1/2/3；实现 θe/ωe；833Hz 验收；宿主 → 台架 → 全量回归。
3. **P0-B 独立速度观测器**：15kHz（或 30kHz 独立状态）；低速纹波全局变量；速度环验收。
4. **P1 位置前馈**：估计器稳定后；`inertia_eff` 默认 0，手动/自整定两条路径。
5. **P1 限幅/保护**：先观测/告警，再谈动作。
6. **P2 输入轨迹/电流环**：按应用需要排序。
7. **P3 可选**：有实测收益再做。

门禁：每一阶段独立验收；不同时改多个环；不 commit/不烧录，除非 Kaiser 明确要求。

## 6. 测试与验收

- 宿主：`encoder/foc/control/identify/params` 全量；新增 FOC 角度/速度观测器/前馈/限幅的纯算法测试；`make artifacts` 无警告；`git diff --check`；热路径反汇编无 Flash 调用。
- 台架：`cal encoder`/`cal rl`/摩擦/位置命令；`foc trace`；`[ISR]`/`[MOTION]` 统计；低速 1/10/100rpm（输出端；100rpm 为有意上限，约 833Hz 电频率）；速度阶跃；位置阶跃/跟随；负载/减速器；母线/温度（如可测）。
- FOC 电角度：100rpm 下 θe 误差/相电流稳定性/`cal encoder`/`cal rl`；候选 1/2 对比。
- 速度：低速纹波、阶跃、相位；位置：完整 vernier 的跟随/回差/保持。
- 调试全局（仅调试，不进正式控制）：`g_motion_speed_dbg_rad_s`、`g_motion_speed_ripple_pp_dbg`、`g_motion_speed_ripple_max_dbg`（`.noncacheable.bss`）；1kHz 窗口/速度拍更新；Ozone/RTT 直读；不新增长捕获/命令。变量命名/窗口/清零在 plan 定。
- 回归：现有 5 套宿主测试、构建、热路径；`cal` 结果不被新观测器破坏。

## 7. 风险登记

| 风险 | 影响 | 缓解 | 决策 |
|---|---|---|---|
| P0-A PLL 在 833Hz 相位滞后/角度误差 | Park 角错误/电流不稳 | 候选 1（原始 θe + ωe 观测器）优先；833Hz 验收；候选 2 单独验证 | 待定 |
| P0-A 编码器坏帧/跳变/零点 | 电角度错误 | 现有 hold/降级；`cal encoder`；不把错误角送 Park | 待定 |
| 30kHz ISR 负载 | 挤占电流环预算 | 先测 cycles；只做必要运算；P0-A 不改 Park 位置滤波 | 待定 |
| P0-B 带宽过高/过低 | 噪声/相位滞后 | 15k 独立观测器；算法 spike + 台架扫带宽；LPF 降级 | 待定 |
| 两个观测器职责边界 | 电角度限速与速度环不一致 | 明确 FOC ωe 与 speed 各自的消费者；调试口径 | 待定 |
| 反转/齿隙/扭振 | 速度/角度误判 | 方向、首拍、保持、限幅；台架低速往返 | 待定 |
| 位置模式完整 vernier | 回差/精度 | 位置模式不用 rotor-only；完整解算；精度验收 | 待定 |
| `inertia_eff` 误差 | 位置超调/电流冲击 | 默认 0；手动 YAML；自整定后续；限幅 | 待定 |
| 100rpm 超额定 | 电机/驱动风险 | 有意上限；短时/受控测试；温度/母线监控 | 已定（有意） |
| 参数迁移 | 丢失 motion/pos/friction 整定 | software 记录版本化；不静默丢 | 待定 |
| 保护动作误开 | 停机/硬件风险 | M1 只检测；单独评审 | 待定 |
| 旧常量/注释误导 | 调参/验收误判 | §4.7 先清理 | 待定 |

## 8. 决策与待定

### 8.1 已定（Kaiser/本会话）

- 保留 30k/15k/10k；三相采样；8MHz 编码器；50:1 减速器；单圈位置。
- ODrive 只作设计参考，不移植代码/架构；不照抄数值。
- **FOC 电角度观测器首要保证 θe 精准可靠；速度观测器可以与电角度观测器独立。**
- **速度环使用独立、更快的观测器是合理的；FOC 电角度观测器不与其共享状态/带宽。**
- **ωe 观测输出同时供 FOC 与速度环使用（合并方案）：P0-B 输出 ωe + speed；P0-A 只保留 raw θe + 延迟/插值。**
- **速度/力矩模式只用 rotor 编码器；位置模式必须完整解算 rotor+vernier 并保证精度。**
- **hold/fallback 按 15kHz 控制周期派生（20ms/500ms → 300/7500），保持 15kHz，不降频。**
- **低速纹波用调试全局变量观察，仅调试使用。**
- **观测器参数放 `config/software.yaml` 的 software 域。**
- **`inertia_eff` 默认不启用前馈；YAML 手动填入 + 自整定两条路径。**
- **100rpm 输出上限是有意的，不改。**
- 估计器优先；先讨论估计器，再进入实现计划。
- 不在本轮 commit/push/烧录，除非明确要求。

### 8.2 Mephisto 建议（待确认）

- P0-A 优先候选 1 + 候选 3：θe 走原始转子角 + offset；观测器只估 ωe；再叠加输出延迟/插值补偿（当前不足 #1/#2 的直接修复）；候选 2（PLL 同时估 θe/ωe）仅在 833Hz 验收通过后考虑。
- P0-B 独立速度观测器放 15kHz 速度拍；二阶 PLL/跟踪结构；`velocity_lpf_coeff` 先保留为降级路径。
- 按合并方案：P0-B 为单一 speed/ωe 观测器；P0-A 只保留 raw θe + 延迟/插值；FOC 解耦/限速/遥测用 P0-B 的 ωe。
- P1 位置滤波增加 `accel × inertia_eff` 前馈；`inertia_eff` 默认 0，手动 YAML 先行，自整定后续。
- P1 限幅/保护先观测/告警，后动作。
- P2 输入轨迹/电流环细节按应用收益排序。
- P0 先清理 §4.7 的口径/常量。

### 8.3 待定（需要 Kaiser/实测）

- P0-A 候选 1/2/3、是否叠加延迟/插值补偿、ωe 带宽/更新率、`foc_angle` 100Hz LPF 的处理。
- 合并后 P0-B 的更新率（30k/15k）、ωe 质量/带宽；P0-A 的 raw θe 延迟/插值。
- P0-B 15k/30k、结构、带宽、`velocity_lpf_coeff` 的移除/保留。
- 两个观测器的编码器故障/保持/反转降级语义。
- 位置模式完整 vernier 的精度/回差/保持验收。
- `software.yaml` 的 `control.foc_observer`/`control.speed_observer` 字段/默认值；flash 迁移/版本。
- `inertia_eff` 自整定路径/工具；100rpm 测试的时长/温度/母线/负载条件。
- 限幅/保护（overspeed/spinout/热限流）的开启边界；电流环 R·I/相位补偿是否进入本轮。

## 9. 参考

- ODrive v3.x：`Firmware/MotorControl/controller.cpp`（`Controller::update`、`POS_FILTER`、限幅/保护）、`controller.hpp`（默认值）、`motor.cpp`（R/L→PI、`Idq`、`current_lim`、前馈）、`foc.cpp`（`FieldOrientedController`、0.80 调制、相位时间戳）、`encoder.cpp/.hpp`（PLL、插值、标定）。
- ODrive 文档：`https://docs.odriverobotics.com/v/latest/manual/control.html`、`.../changelog.html`；ODrive Pro 规格页（24kHz PWM、8kHz 控制环、24kHz 电流环 coming soon）。
- 本工程：`App/Control/Src/app_motion.c`、`App/Algorithm/Src/algo_motion.c`、`App/Control/Src/app_foc.c`、`App/Control/Src/app_foc_current.c`、`App/Algorithm/FOC/Src/foc_current.c`、`foc_angle.c`、`App/Platform/Src/app_encoder.c`、`app_motion_timer.c`、`config/software.yaml`、`config/hardware.yaml`、`config/motor.yaml`、`App/Control/Inc/app_protect_policy.h`、`App/Platform/Inc/app_param_record.h`。
- 版本注意：ODrive v3.x 已 NRND；Pro/S1 固件闭源；0.6.12 文档与 v3.x 源码默认值可能不同（如 `input_filter_bandwidth` 2→20）。
