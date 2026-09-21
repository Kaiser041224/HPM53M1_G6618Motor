# YHorizon-JM FOC 控制算法参考副本

> 本目录是**只读参考资料**，不参与本工程构建（`Doc/` 不在 `CMakeLists.txt` 的源文件与 include 路径内）。
> 用途：为本工程 FOC（`App/Algorithm/FOC/` + `App/Control/`）提供一份外部实现对照，重点是
> **电角度自校准、绝对位置解算、级联控制律** 三块的另一种做法。
>
> ⚠️ **本目录含 GPLv3 代码**，取用前务必先读第 1 节与 `SOURCE_NOTICE.md`。

---

## 1. 上游出处与许可证（先读这一段）

| 项 | 值 |
| :--- | :--- |
| 上游仓库 | `D:\Codes\GitHub\YHorizon-JM`（开源一体化关节电机，作者 YuanWeitian） |
| 上游提交 | `800313139bd607c36b6641574cba9738a1f34f90` |
| 上游路径 | `firmware/Foc/**`、`firmware/App/{config.h,servo.*,cali.*,cali_nv.*}` |
| 许可证 | **GPLv3**（`LICENSE-GPL-3.0.txt` 为上游全文副本） |
| 复制方式 | **逐字复制，零修改**（17 个文件，sha256 逐个比对一致，见 `SOURCE_NOTICE.md`） |

**三条必须遵守的结论：**

1. 本工程固件按 `AGENTS.md` 采用 **BSD-3-Clause**。GPLv3 代码一旦被**编译进**本工程或与之链接，整个产物
   就要按 GPLv3 分发。因此：**只作阅读参考，不要把 `reference_src/` 加入构建**（本目录按此设计，
   没有任何构建文件引用它）。若要把某个算法落进本工程正式代码，必须**重写实现（clean-room）**，
   只借鉴思路与公式，不复制代码文本。
2. 上游 `LICENSE` 在 GPLv3 之上附加了"禁止商用"。GPLv3 §7 明确允许接收者删除此类附加限制，
   即该"禁止商用"对上游的 `firmware/`、`sdk/`、`tools/` **本身就不成立**。所以不能指望
   "上游禁商用"来约束你——真正的约束是 GPLv3 的传染性。
3. 上游自有源文件**没有任何版权头/SPDX 标识**，`LICENSES/` 也未集中收录第三方全文。因此本副本
   保留原文件名与内容原样，未加任何本工程版权头——**不要**给它套 `Alliance HardwareGroup` 头。

`host_test/` 下的验证脚本是在本工程内新写的，但它编译并链接上游 GPLv3 源文件，故一并按
**GPLv3** 分发（见 `host_test/README.md`）。

---

## 2. 算法总览

上游是**电压内环默认、电流内环可选**的三模式伺服。算法分两层，频率不同：

```
                     ┌──────── TIMER5  4 kHz  控制外环 (servo.c) ────────┐
                     │  SyncVernier()        游标绝对位置在线重锚        │
 编码器原始角 ──►     │  三模式外环 → t_ref  (MOTION/VELOCITY/POSITION)  │
                     │  位置环: 梯形限速 + PD/PID 混合 + 反算抗饱和      │
                     └───────────────────────┬─────────────────────────┘
                                             │ s_t_ref (pu)
                     ┌───────────────────────▼──── TIMER0  20 kHz ───────┐
 编码器原始角 ──►     │  Foc_EncoderUpdate/Predict → mech_unwrapped, θe, │
                     │                              vel_hat             │
 两相电流     ──►     │  Foc_CurrentStep: Clarke→Park→(可选 d/q PI)      │
                     │  Foc_ApplyDq: LimitDQ → 斜坡 → InvPark → SVPWM   │
                     └───────────────────────┬─────────────────────────┘
                                             │ duty_a/b/c → 三相桥
```

关键设计取向：**外环输出 `t_ref` 是"电压指令"量纲（pu，1.0 = PWM 满幅）**，无论内环是电压还是电流。
电压内环下 `t_ref` 直接当 `Vq` 用；电流内环下才映射成 `Iq_ref`（`servo.c:249`）。这个"统一以电压为
中间量纲"的取舍与上游默认配置一致，但也是它最主要的短板（见第 6 节）。

---

## 3. 逐模块梳理

### 3.1 `Foc/foc_math.{c,h}` —— 坐标变换与三角函数（纯数学，零依赖）

编译依赖只有 `<math.h>` + `<stdint.h>`。可原样用于任何平台。

**常数与约定**（`foc_math.h:10-14`）：

| 符号 | 值 | 说明 |
| :--- | :--- | :--- |
| `FOC_TWO_PI` / `FOC_PI` | 6.283185… / 3.141592… | — |
| `FOC_SQRT3_2` | 0.8660254 | √3/2 |
| `FOC_INV_SQRT3` | 0.5773503 | 1/√3 |
| `FOC_PWM_NEUTRAL_DUTY` | 0.5 | 零矢量占空比 |

**Clarke（`foc_math.c:93-100`）—— 只有两相输入**，是三相形式的等值简化：

```c
alpha = ia;
beta  = (ia + 2*ib) * (1/√3);
```

在 `ia + ib + ic = 0` 约束下与三相显式式 `α = (2/3)(ia − ib/2 − ic/2)`、`β = (ib − ic)/√3`
**代数完全等价**。上游只用两电阻（INA2181 双通道）+ 注入组采样，所以省掉了第三相与一次减法。
本工程是三电阻采样，直接用三相形式即可，**不必移植这一处**。

**Park / 反 Park（`foc_math.c:102-126`）**：标准形式，与上游 spec 的符号约定一致
（`d = α·cosθ + β·sinθ`，`q = β·cosθ − α·sinθ`）。

**`Foc_LimitDQ`（`foc_math.c:128-144`）—— 优先 d 轴的圆形限幅**：超限时先钳 `d`，再把剩余的
幅值预算给 `q`：

```c
if (d² + q² <= max²) return;
d = clamp(d, ±max);
q = clamp(q, ±sqrt(max² − d²));
```

与上游 spec §3.4 第 4 步"保角缩放 `v_max/mag`"**语义不同**：这里是"保 d 轴、牺牲 q"，
他们的做法是等比例缩放（保角）。`i_d = 0` 的常规工况下两者都不触发；一旦弱磁/MTPS 介入，
语义差异会显现——**移植时注意选择**。

**`Foc_SinCos`（`foc_math.c:146-172`）—— 256 点查表 + 线性插值**：表长 257（多一格避免 `idx+1`
越界），`sin` 用 `idx + 3N/4` 的相位搬移复用同一张表。我在主机上实测最大绝对误差
**7.53e-5**（对 20 万点扫描，含越界输入），远超电流环所需精度。

查表的**正交性残差** `max|c²+s²−1| = 1.51e-4`，因此 Park → 反 Park 往返会留下约
1.5e-4 的相对误差——但它是**缩放误差而非旋转误差**（推导：`α' = α(c²+s²)`），
对 FOC 完全无影响。`host_test/` 里把这一项单独测出来了，就是为了避免把它误判成坐标变换的 bug。

> 上游 spec §3.2 计划"先用 `sincosf()`，若预算超限 V2 换 1024 点表"。这份实现就是那条退路的
> 现成答案，而且只要 257 个 float（约 1 KB，可放 DLM）。

**`Foc_ApplyEncoderDirToMechTheta`（`foc_math.c:75-86`）—— 方向镜像**：`dir < 0` 时
`θ → 2π − θ`。注意它**把"方向"折进了机械角**，而不是像上游 spec 那样写成 `p·dir·θm`。
两种写法等价，但会影响偏置的符号（见第 5 节陷阱 2）。

### 3.2 `Foc/foc_svpwm.{c,h}` —— min-max 零序注入调制

`foc_svpwm.c:9-34`，全部内容就是：

```c
ua =  u_alpha;
ub = -0.5*u_alpha + (√3/2)*u_beta;
uc = -0.5*u_alpha - (√3/2)*u_beta;
offset = -(max(ua,ub,uc) + min(ua,ub,uc)) * 0.5;      /* 零序注入 */
duty_x = clamp01(u_x + offset + 0.5);
```

**与上游 spec §3.5 的 `foc_modulation` 是同一种方法**（min-max 零序注入，等价 SVM、免扇区判断），
公式逐项一致。差异只有两处，都是本工程应当保留自己版本的：

| 项 | YHorizon-JM | 本工程 spec |
| :--- | :--- | :--- |
| 采样窗口限幅 | **无** | 有：`span ≤ 0.77·v_bus`，`duty_max = 0.885` |
| 母线欠压门控 | **无** | 有：`v_bus < v_bus_min` 返回 −1 |
| 输出量纲 | 直接占空比 `[0,1]` | 占空比 + `v_scale_out` 诊断 |

上游是**两电阻采样、在 PWM 谷底采样**，所以不需要压缩调制范围去保证采样窗口；本工程是三电阻，
`duty_max = 0.885` 的 11% 调制范围损失是必要代价。**这块不必移植。**

我在主机上验证过它的正确性（本目录 `host_test/`）：

- 0.5 pu 圆轨迹下占空比跨度恰为 **√3·A = 0.8660**，无任何削顶；
- 零序注入不变量 **`max(duty) + min(duty) ≡ 1`** 逐点误差为 **0**；
- 线性区上限确在 **`1/√3 = 0.5774 pu`**：A = 0.5769 仍严格线性，A = 0.5874 时跨度被压缩
  （实测 1.0000 vs 理论 1.0173）并出现钳位——说明上游 `CFG_V_LIMIT = 0.50` 这个值选得有依据。

> **一个容易写错的判据**：零序注入正确的判据是 `max(duty) + min(duty) = 1`，**不是**
> "三相占空比之和 = 1.5"。因为 `Σu = 0`、`Σduty = 3·offset + 1.5`，而
> `offset = −(max+min)/2 ≠ 0`；只有 `max+min = 0` 的特殊相位三者和才等于 1.5，
> 一般相位下它在 [1.125, 1.875] 之间变化（本测试首版就是按错误的判据写的，被测试自己抓出来）。
> 本工程 spec §3.5 第 4 步写的是正确形式（`max(d)+min(d) = 1`），无需修正。

### 3.3 `Foc/foc_encoder.{c,h}` —— 角度链 + 速度估计 + 读失败降级

这是**与 `App/Algorithm/FOC/foc_angle` 最直接对照**的模块，但内容更厚。

**数据结构**（`foc_encoder.h:20-42`）核心字段：`pole_pairs`、`direction`、`electrical_offset_rad`、
`mech_wrapped`、`mech_unwrapped`（多圈展开）、`mech_zero`、`last_elec`、`vel_hat`、`elec_angle`，
外加一组二阶 LPF 的系数与历史。

**`Foc_EncoderUpdate`（`foc_encoder.c:159-190`）—— 每拍主更新**：

```c
aligned = ApplyEncoderDirToMechTheta(raw_mech, direction);   /* 含方向镜像 */
delta   = wrapToPi(aligned - mech_wrapped);                  /* 单圈增量，天然跨零 */
mech_wrapped   = aligned;
mech_unwrapped += delta;                                     /* 多圈累计 */
elec_angle = wrap2pi(pole_pairs * aligned + electrical_offset_rad);
omega_e    = wrapToPi(elec_angle - last_elec) / dt;           /* 差分，含方向符号 */
vel_hat    = LPF_2nd_Butterworth(omega_e) / pole_pairs;       /* → 机械角速度 */
```

三个值得注意的设计：

1. **`elec_angle` 只由 `aligned`（原始编码器角，含方向镜像）与 `offset` 算出，不经过 `mech_zero`。**
   上游的 `SET_ZERO` 只写 `mech_zero`，仅影响 `Foc_EncoderGetPosition()`（对外位置）。
   也就是**上位机改零点不会破坏换相**——这恰好就是上游 spec §3.3 明确要求的
   "必须使用未加软件零点的原始角"。这条约束这份实现天然满足。
2. **速度用二阶巴特沃斯低通，不是一阶**（`foc_encoder.c:7-20`）：双线性变换
   (`K = 2/T`, **不带频率预畸变**)，截止 `CFG_VEL_LPF_HZ = 500 Hz`，在 20 kHz 采样率下计算系数。
   相比上游 spec 的一阶 LPF 100 Hz，二阶在同样衰减下相位滞后小得多，对速度环相位裕度更友好。
   注意 LPF 跑在 **20 kHz 的 PWM 中断里**（`Servo_OnPwmIsr` 调用的），不是在 4 kHz 外环里。
3. **`Foc_EncoderPredict`（`foc_encoder.c:138-157`）—— 读失败降级**：编码器当拍读失败时
   用 `vel_hat` 外推一拍（位置、电角度、绕圈数全部按 `vel_hat·dt` 推进），并且**让 LPF 继续
   对"外推出来的 ωe"做滤波**。这是把一次 SPI 读失败变成"温和的一拍延迟"而不是"位置跳变"。

**`Foc_EncoderSyncAbs`（`foc_encoder.c:107-126`）—— 多圈重锚，也是这块最需要改的地方**：

```c
target = aligned_abs + round((mech_unwrapped - aligned_abs)/period) * period;  /* 取最近的周期倍数 */
err = target - mech_unwrapped;
if (err > π || err < −π) mech_unwrapped = target;      /* ← 只在误差很大时才纠正 */
```

判据方向是**"只接受大跳变"**。配合"快读不带 CRC"（`Encoder_ReadRawFast` 只判 `frame != 0`），
一次坏读会被当成合法的大跳变直接锚定，而且**下一次好读会以错误的新位置为基准继续取整**，
于是偏移**永久保持到重新上电**。我在主机上量化过：把辅助编码器方向配置搞反，残差门限只拒绝
**30%**，被接受的 70% **全部**是"整圈数错误"（单圈角度仍然正确）。详见第 6 节陷阱 4。

### 3.4 `Foc/foc_vernier.{c,h}` —— 30/31 齿游标绝对位置解算

**这是整份副本里对本工程最可能有直接价值的一块**——本工程是双 KTH7823，具备做游标的硬件条件。

原理（`foc_vernier.c:24-70`）：电机端 30 齿、旁置 31 齿啮合，能唯一区分 **31 个电机圈**。
令 `u = raw1/CPR`（电机齿轮的圈内分数）、`v = dir2·raw2/CPR`（旁置齿轮的圈内分数），则

```c
residual = wrap01(v + (N1/N2) * u);     /* N1=30, N2=31 */
n        = round(residual * N2);        /* 唯一圈号 0..30 */
if (|residual*N2 − n| > 0.35) return 0; /* 残差门限：不可信则丢弃 */
*raw_abs = (n + u) * 2π;                /* 电机轴绝对角，范围 [0, 31·2π) */
```

数学上可验证：设总圈数 `T = k + u`，旁置齿轮转过 `−(N1/N2)·T`，则
`residual = frac(−(N1/N2)·k)`，因 `−30 ≡ 1 (mod 31)` 得 `residual = k/31`，故 `n = k`。

**主机实测（`host_test/`，20 万点全窗口扫描）**：

| 指标 | 实测值 |
| :--- | :--- |
| 解码失败（门限拒绝） | **0 / 200000** |
| 最差绝对角误差 | **0.000383 rad = 0.022°** |
| 残差到整数的最大距离 | **0.0036**（门限 0.35，判决边界 0.5） |
| 旁置编码器噪声 ≤ 1.41° | 全部正确，**零滑移** |
| 旁置噪声 = 5.6° | 门限 **100% 拒绝**（失败安全） |
| 旁置噪声 ≥ 7.6° | 可能滑移整圈（实测最差 29 个电机圈） |

结论：**对随机噪声很健壮（失败安全），对"系统性配置错误"不健壮**——这一点非常关键，因为
`CFG_ENC2_DIR`（旁置编码器方向）和 `CFG_ENC2_OFFSET_RAD` 是 `config.h` 里的**编译期常量，固件
从不校准、也不做自检**。方向配反时残差门限只挡住 30%，其余 70% 会给出"单圈角正确、整圈号错"
的位置，表现为上电后输出轴位置偏差 **45° 的整数倍**，且几乎无法自诊断。

本工程若要用这条路线，**必须**补两件事：① 旁置编码器方向纳入辨识（或用一次慢转自动判定）；
② 给周期同步加"连续性判据"，而不是现在的"只接受大跳变"。

### 3.5 `Foc/foc_current.{c,h}` —— d/q 电流调节器（**注意量纲**）

只有 75 行，但有两处移植必须注意的点。

**增益是"模型自配置"而非手调**（`foc_current.c:15-33`）：

```c
kp_v = L * wc;    /* wc = 2π·CFG_CUR_BW_HZ = 2π·500 rad/s */
ki_v = R * wc;
Foc_CurrentSetVbus(cur, vbus);   /* kp = kp_v / vbus,  ki = ki_v / vbus */
```

极点对消法，与上游 spec §3.4 的 `kp = Ls·ωbw`、`ki = Rs·ωbw` **公式完全一致**（他们取 ωbw = 2π·1kHz，
后来台架下调到 500 Hz——**巧合地与上游同值**）。

**`Foc_CurrentSetVbus` 在 4 kHz 控制中断里每拍都调**（`servo.c:272`），用实测母线电压重算增益：
电池从 48 V 掉到 36 V，电流环带宽不变。这是**参数调度式在线自适应**，本工程 spec 未提及，
值得借鉴（他们目前是每拍现算 `v_max`，但增益不随 VBUS 调度）。

**`Foc_CurrentStep`（`foc_current.c:43-75`）**：

```c
dq = Foc_Park(Foc_Clarke(ia, ib), elec_rad);   /* 总是算，run_pi=0 也更新遥测 */
if (!run_pi) { Foc_CurrentReset(cur); return; } /* 只观测：清积分器 */
id_ref = clamp(id_ref, ±i_lim);  iq_ref = clamp(iq_ref, ±i_lim);
∫d += ki*ed*dt;  ∫q += ki*eq*dt;               /* 积分器各自限幅 ±v_lim */
vd = clamp(kp*ed + ∫d, ±v_lim);  vq = clamp(kp*eq + ∫q, ±v_lim);
Foc_LimitDQ(v_lim, &vd, &vq);                  /* 圆形限幅 */
```

**⚠️ 陷阱 1：`vd`/`vq` 是 per-unit（÷Vbus），不是伏特。** `v_lim` 传的是 `CFG_V_LIMIT = 0.5`。
而本工程 spec §3.4 的 `foc_current` 是**伏特域**（`v_max = 0.513·v_bus` 每拍现算）。
直接照搬数值会得到 0.5 V 的限幅——**电机基本不动**。

**⚠️ 陷阱 2：没有解耦前馈与反电势前馈。** 本工程 spec 有 `decoupling_en` +
`vd_ff = −ωe·Lq·iq`、`vq_ff = +ωe·(Ld·id + λ)`（V1 默认关）。上游完全没有这一项。
对上游那个电机无所谓（Rs 5.16 Ω ≫ ωe·L），但**对 G6618 影响很大**：
Rs = 0.158 Ω、Ls = 118.5 µH，在 550 Hz 电频率下 `ωe·L ≈ 0.41 Ω`，是 Rs 的 **2.6 倍**。
所以**解耦前馈在本工程是必需品，不是可选项**——这条差异要特别记住，别被上游的"没有也跑得动"误导。

### 3.6 `App/servo.{c,h}` —— 三模式级联控制律

这块是**本工程 V2/V3 外环的直接参考**（他们 V1 只有电流环，速度/位置环明确后置）。

**`Servo_OnCtrlIsr`（`servo.c:265-358`，4 kHz）三种模式**：

| 模式 | 控制律 | 代码 |
| :--- | :--- | :--- |
| `MOTION` | `t_ref = Kd·(v_set − v_act) + Kp·(p_set − p_act) + t_ff`，钳位 ±V_LIMIT | `servo.c:291-295` |
| `VELOCITY` | 速度 PI（积分钳位 ±V_LIMIT） | `servo.c:296-299` |
| `POSITION` | 位置环 → `v_ref` → 速度 PI 内环，**内环饱和时冻结外环积分** | `servo.c:300-357` |

几个细节做得比一般实现细：

- **位置环是"梯形限速 + PID"按误差权重混合**（`servo.c:302-349`）：
  ```
  v_lim  = min(v_max, sqrt(2·a·|ep|))        /* 减速包络，a = CFG_POS_ACC_DEFAULT */
  v_prof = sign(ep) · v_lim
  w      = clamp(|ep| / settle, 0, 1)         /* settle = CFG_POS_SETTLE_RAD */
  v_ref  = w·v_prof + (1−w)·v_pid             /* 远处走包络，近处交给 PID */
  ```
  这是避免"纯梯形终点冲击"和"纯 PID 大误差超调"的折中，比单一策略稳。
- **反算抗饱和（back-calculation）**（`servo.c:314, 351-356`）：先试算内环未饱和输出，
  若会饱和就把外环积分恢复成上一拍的值。这是教科书做法，很多实现只做积分钳位。
- **电压斜坡限制**（`servo.c:83-96, 132-142`）：`CFG_V_SLEW_PU_S = 100 pu/s`，
  在电压/开环路径上生效；**电流内环路径上不生效**（电流环自己限流）。
- **`Servo_OnPwmIsr` 里的模式分派**（`servo.c:215-263`）：20 kHz 每拍都读编码器、算电流、
  出 SVPWM，与上游 spec "角度、电流、计算、写占空比同拍完成"的纪律一致。

**需要留意的一处不一致**：`Servo_SetGains`（`servo.c:452-456`）**没有加临界区**，而
`Servo_SetVelocityGains`/`Servo_SetPositionGains` 都加了 `__disable_irq()`。虽然 Cortex-M4 上单个
float 存/取是原子的，最坏只是一拍里 Kp 新、Kd 旧，但风格上不统一。

### 3.7 `App/cali.{c,h}` + `App/cali_nv.{c,h}` —— 自校准与持久化

这是上一轮讨论的重点，完整流程与实测结论见第 4 节。模块职责：

- `cali.c`：校准流程与状态机。`Cali_Start()`（上电入口）→ 有 NV 走 `Cali_ApplySaved()`，
  无 NV 走 `Cali_RunCommand()`；`Cali_Run()` 是核心算法；阶段进度通过 CAN `0x3C0` 上报。
- `cali_nv.c`：Flash 记录读写。定长记录（v3 = 40 B）+ magic + version + CRC32 +
  **写后回读校验**；带 v1/v2 → v3 迁移。
  ⚠️ 这一对文件**是硬件耦合的**（`fmc_*`、`__disable_irq`、CAN 停机），列入副本只为
  "记录布局 + 校验纪律"的参考，不属于"FOC 算法"。

---

## 4. 电角度自校准：逐阶段拆解与实测

`Cali_Run()`（`cali.c:241-430`），总时长按配置常量推算 **≈17 s**。

| 阶段 | 动作 | 参数 | 代码 |
| :--- | :--- | :--- | :--- |
| ① 强制对齐 | 开环 `Vd = 0.1 pu`、电角度锁 0 | 300 ms | `cali.c:260-263` |
| ② 正向旋转采样 | 电角速度 +π rad/s，每 10 ms 采一点，跳过前 250 ms | 8 s | `cali.c:285` |
| ③ 反向旋转采样 | 电角速度 −π rad/s，同上 | 8 s | `cali.c:293` |
| ④ 判定 | 方向 / 极对数 / 运动量检验 | — | `cali.c:300-333` |
| ⑤ 力矩方向探针 | 施加 `Vq = 0.05 pu`，实测机械位移符号定 `closed_loop_dir` | 400 ms | `cali.c:369-419` |
| ⑥ 落盘 + 回读 | 写 NV 并校验 | — | `cali.c:493-522` |

**偏置解算用"圆周均值"，不是单点采样**（`cali.c:146-166` 累加、`cali.c:345-359` 解算）：

```c
/* 每个采样点： off = wrapToPi(elec − pp·aligned) */
Foc_SinCos(off, &s, &c);  acc.sin_p += s;  acc.cos_p += c;   /* 同时累 +1 / −1 两种极性假设 */
...
offset = wrap2pi( atan2(acc.sin_p, acc.cos_p) );             /* 约 775×2 个样本的矢量平均 */
```

**为什么"双向 + 圆周均值"是关键**（以下为我推导，上游文档未解释）：单方向旋转时，
`off` 的误差不只来自编码器量化，还来自**一切与运动方向相反的阻力矩**造成的角度滞后：

- 干摩擦/粘滞阻力 → 正程滞后 `+δ`、反程滞后 `−δ`；
- **齿槽转矩也一样**：齿槽是*位置*相关的静力矩，自身不随方向变号，但它始终是**阻力**，
  于是正程滞后、反程超前，同样变成 `±δ`。

两程样本数几乎相等（各约 775），对这两簇等量样本取圆周均值，几何上正落在弦的中垂线 →
**`offset_true`，`±δ` 一次项精确抵消**。这解释了为什么**它不做齿槽补偿也能得到不错的电角度**：
它把齿槽当"可抵消的方向性扰动"处理掉了。这一步是整份代码里水准最高的设计。

**但有三处真实缺陷：**

1. **好估计器有条件才启用**（`cali.c:345`）：
   ```c
   if ((offacc.n > 8U) && (pole_pairs == (uint8_t)CFG_POLE_PAIRS))
   ```
   一旦极对数不等于编译期的 14，就退化成 `offset = wrap2pi(−pp·theta0)`——**静止单点采样**。
   静止点位置由齿槽决定，等于把刚抵消掉的齿槽影响全带回来，且**没有任何提示**。换电机的人
   会静默拿到更差的偏置。
2. 偏置是**单一常数**，不建模编码器 INL、磁铁偏心、`pp·θm` 的高次谐波。
3. 全程**零负载、近零速**（12.9 °/s 机械），无法看到任何负载相关行为。

**探针验证（阶段⑤）值得单独表扬**：校准完偏置后并不直接相信符号，而是施加 0.05 pu 的 Vq
持续 400 ms、积分机械位移，**用实测确认"正力矩确实让输出正转"**。很多实现是靠推理定符号的。

**与上游 spec 的 `id_encoder` 对照**（各有胜负，建议取并集）：

| 维度 | YHorizon-JM `cali.c` | 本工程 spec `id_encoder` |
| :--- | :--- | :--- |
| 激励方式 | **开环电压** `Vd = 0.1 pu` | **闭环电流** `i_d = I_cal = 2 A` |
| 扫描方式 | 连续旋转，正反各 8 s，10 ms/点 | 步进 180 步 × 5 ms，正反各一遍 |
| 样本量 | 约 1550 点 | 360 点 |
| 解算 | 圆周均值 `atan2(Σsin, Σcos)` | 同（方法一致 👍） |
| 方向判定 | 由正程机械位移符号 | 强制角 +60° 后测 Δθm 符号 |
| 极对数校验 | `pp_f = 电角/机械角`，容差 ±0.40 | 一电周期应转 2π/p，偏差 >±20% 报警 |
| **力矩方向验证** | **有**（Vq 探针 400 ms，实测） | 可选（i_q=+1 A，1 s，不判失败） |
| **质量指标** | 无 | **有**：`q = |Σ|/N`，阈值 0.95 |
| **闭环残差验证** | 无 | **有**：均值 |δ| < 5°、最大 < 15° |
| 耗时 | ≈17 s | ≈3.2 s + 0.5 s |
| 自由旋转需求 | 需（0.286 机械圈/程） | 需（电角 2π = 机械 36°） |
| 持久化 | **写 Flash**（含版本迁移） | V1 仅 RAM |

> ⚠️ **绝对不要照搬上游的激励方式。** 上游用 `0.1 pu` 开环电压是**因为它电机相电阻 5.16 Ω**：
> 0.1 pu × 24 V = 2.4 V → 2.4/5.16 ≈ 0.47 A，安全。**G6618 的 Rs 只有 0.158 Ω**，
> 同样 0.1 pu 会给出 **≈15 A**，探针 0.05 pu 也有 **≈7.6 A**——远超他们 V1 的 `i_trip_a = 10 A`。
> **本工程必须保持闭环电流激励**，这是正确的选择。

**建议的取用方式**：保留本工程的步进扫描 + 质量指标 + 闭环残差验证（这三项上游没有），
从上游补三点：① 力矩方向探针做成**强制检查项**；② 连续双向旋转 + 大样本圆周均值
（噪声抑制强于 180 个离散点，且天然抵消摩擦/齿槽一阶影响）；③ 校准结果落 Flash 的
写后回读纪律（本工程 `app_param.c` 已有类似机制，核对是否覆盖字段级回读）。

---

## 5. 与宿主工程现有模块的对照

| `reference_src/` | 本工程对应 | 关系与差异 |
| :--- | :--- | :--- |
| `Foc/foc_math.{c,h}` | `App/Algorithm/FOC/foc_math.h` | 同类。Clarke 两相 vs 三相（等价，不必移植）；**`Foc_LimitDQ` 保 d 轴 vs 他们保角**；**offset 符号相反**（陷阱 3）；`Foc_SinCos` 257 点表实测 7.5e-5，是他们"预算超限时的 V2 退路"现成答案 |
| `Foc/foc_svpwm.{c,h}` | `foc_modulation.{c,h}` | 同一方法（min-max 零序注入），公式一致。**不必移植**：本工程有采样窗口限幅与欠压门控，上游没有 |
| `Foc/foc_encoder.{c,h}` | `foc_angle.{c,h}` + 速度估计 | 上游含多圈展开、游标绝对种子、**二阶巴特沃斯 500 Hz 速度滤波**、读失败外推；本工程 V1 是一阶 100 Hz、无多圈。**二阶滤波与 Predict 降级值得借鉴** |
| `Foc/foc_vernier.{c,h}` | 无对应（但本工程有双 KTH7823） | **最值得评估的一块**，可让上电即得绝对多圈位置，省掉回零动作 |
| `Foc/foc_current.{c,h}` | `foc_current.{c,h}` | 同为 Lωc/Rωc 极点对消。**量纲不同（pu vs V）**、**上游无解耦前馈**（对 G6618 是硬伤）、抗饱和方式不同（钳位 vs 他们的衰减+保角） |
| `App/servo.{c,h}` | 无对应（V1 只有电流环） | **V2/V3 外环的直接参考**：三模式、位置梯形/PD 混合、反算抗饱和、电压斜坡 |
| `App/cali.{c,h}` | `id_encoder.c` + `app_motor_identify.c` | 目标相同（电角度零点/方向/极对数校验）。差异见第 4 节表；**双向圆周均值**与**探针验证**是上游长处 |
| `App/cali_nv.{c,h}` | `App/Platform/app_param.c` | 都是 flash 持久化（magic + CRC32 + 回读）。上游多了**版本迁移**（v1/v2→v3），本工程可评估是否需要 |

---

## 6. 移植陷阱（必读）

**陷阱 1 —— 电流环量纲：pu vs V。**
上游 `vd/vq` 是 per-unit（÷ Vbus），限幅值 `0.5`；本工程是伏特域，限幅值 `≈0.513·v_bus`。
照搬数值会导致限幅只有 0.5 V。

**陷阱 2 —— 速度/位置量纲是"输出轴"。**
上游所有对外位置/速度都除了减速比（`CFG_GEAR_RATIO = 8`，`Servo_MotorToOut`，
`config.h:109-116` 的默认增益也都除了 8）。**本工程是直驱（转子 1:1）**，不要把这层缩放带进来。

**陷阱 3 —— 电角度偏置符号相反。**
- 上游：`θe = wrap(pp · aligned + offset)`，其中 `offset = atan2(Σsin, Σcos)`，
  而累加的是 `off = wrapToPi(elec − pp·aligned)`。
- 本工程 spec：`θe = wrap(p · dir · θm_raw − offset)`，`offset = −atan2(Σsin, Σcos)`。

两者各自自洽，但**同一个物理量的正负号定义相反**。混用会出现"偏置是真实值的相反数"，
现象是电机锁定在错误电角度、转矩周期性脉动。移植时必须先确定用哪套约定。

**陷阱 4 —— 多圈重锚判据是反的，且快读没有 CRC。**
`Foc_EncoderSyncAbs` 只在 `|err| > π` 时纠正 = "只接受大跳变"。配合无 CRC 的
`Encoder_ReadRawFast`（只判 `frame != 0`），一次坏读会被当作合法大跳变锚定，而且
**下一次好读以错误位置为基准继续取整**，偏移**永久保持到重新上电**。
本工程要用游标，务必改成"连续性判据"（只接受小误差，或要求连续 N 拍一致），
或用带 CRC 的读法（注意：上游带 CRC 的 `Encoder_ReadMtFrame` 会关中断，不能放进高频中断）。

**陷阱 5 —— 无 NaN/Inf 防御。**
上游全链路没有非有限值检查；本工程 spec §3.1 明确要求"NaN/Inf → 保持上一步输出 + 置
`_fault` 标志"。**保留本工程的做法**，别被上游带偏。

**陷阱 6 —— 解耦前馈对 G6618 是必需的。**
上游没有该项且能跑，原因是其 `Rs = 5.16 Ω` 远大于 `ωe·L`。G6618 反之
（`Rs = 0.158 Ω`，`ωe·L ≈ 0.41 Ω @550 Hz`，**2.6 倍**）。本工程 spec 的
`decoupling_en` 应尽早打开验证。

**陷阱 7 —— 电压内环 = 没有电流保护。**
上游默认 `CFG_INNER_LOOP_VOLTAGE`，此时 `CFG_I_LIMIT_A` 只用于把 `t_ref` 映射成 `Iq_ref`，
**从不与实测电流比较**；唯一约束是被动的 `0.5·Vbus/R`。本工程 V1 只有电流环，**不要**
为了"简单"退回电压环。

---

## 7. 已知局限与缺陷清单

从代码里核出来的，取用时请逐条评估：

| # | 项 | 位置 | 影响 |
| :--- | :--- | :--- | :--- |
| 1 | 多圈重锚只接受大跳变 | `foc_encoder.c:107-126` | 坏读→整圈偏移且永久保持（陷阱 4） |
| 2 | 20 kHz 快读不做 CRC | `foc_encoder.c` 依赖的 `Encoder_ReadRawFast` | 单拍电角度可能错（上游 `encoder.c`） |
| 3 | `CFG_ENC2_DIR`/`OFFSET` 是编译期常量，不校准不自检 | `config.h:38-39` | 配反时 70% 概率静默给出整圈错位（见 3.4） |
| 4 | 圆周均值有条件启用 | `cali.c:345` | 换电机后静默退化为单点采样 |
| 5 | 无齿槽补偿、无死区补偿、无编码器非线性标定、无 R/L/Ke 辨识 | 全库 grep 零命中 | 见第 8 节 |
| 6 | 无运行时保护（唯一故障位是"校准失败"） | `comm.c:39` | 无过流/过压/过温/堵转 |
| 7 | 无命令超时（保持最后一帧） | 上游协议文档明示 | 上位机断开后电机继续运行 |
| 8 | `Servo_SetZero` 不清 `s_v_set`/`s_t_ff` | `servo.c:560-570` | MOTION 模式下 `Kd>0` 时置零后自走 |
| 9 | 硬件故障不关断输出（Fault handler 裸 `while(1)`，无看门狗，break 输入关闭） | 上游 `irq.c`/`board.c:168` | 跑飞即持续驱动 |
| 10 | `Servo_SetGains` 缺临界区 | `servo.c:452-456` | 最坏一拍 Kp/Kd 不同步（轻微） |

---

## 8. 关于"齿槽效应优化"

**明确结论：上游这份代码没有任何齿槽补偿，也没有转矩脉动抑制。** 证据：

- grep `cog|harm|comp|identif|tune|deadtime|ripple|friction|flux|table|lut` 遍及 `App/`、`Foc/`、`Hw/`，
  命中只有三类：注释里的 "compute"；`foc_math.c` 的 **sin/cos 加速查表**（数学加速，非补偿表）；
  `board.c:164` 的**死区时间配置**（只设值，不补偿）。
- 更硬的证据是 **NV 记录布局**（`cali_nv.h:10-24`）：齿槽补偿必须有"角度索引的持久化表"，
  而记录里只有 `pole_pairs / encoder_dir / closed_loop_dir / node_id / user_gains /
  electrical_offset_rad / mt_zero_rad` 这 7 类**标量，一个数组都没有**。
- `servo.c:291-357` 的 `t_ref` 只有 PID + 前馈，无 `+ comp(θ)` 项。

同一家族的其他成员也全缺：死区补偿、编码器 INL/偏心标定、R/L 在线辨识
（`CFG_MOTOR_R_LL_OHM = 10.32`、`L_LL = 4.76e-3` 是 WK4310 手册值直接硬编码，固件从不测）。

本工程 spec §1.3 同样把"死区补偿、齿槽补偿"列为"结构预留、默认关闭"，**两者取向一致**。
若将来要做，上游这份代码**没有可借鉴的辨识流程**，但挂点位置与本工程相同：把角度索引的
前馈量加到 `t_ref`（本工程在 `foc_current` 的给定侧或 `foc_modulation` 之前）。
注意本工程 25 kHz 电流环对高阶齿槽谐波的采样能力优于上游的 4 kHz 外环——
**在这方面本工程架构更有优势，不需向下游对齐。**

---

## 9. 目录内容

```
Doc/ref_code/
├── README.md                  ← 本文件（算法梳理与对照）
├── SOURCE_NOTICE.md           ← 出处、逐文件 sha256、许可证义务
├── LICENSE-GPL-3.0.txt        ← 上游 GPLv3 全文（合规必需，勿删）
├── reference_src/             ← 逐字复制，零修改（17 个文件）
│   ├── Foc/                   ← 纯 FOC 算法层（仅依赖 <math.h>/<stdint.h> + config.h）
│   │   ├── foc_math.{c,h}         坐标变换、wrap、257 点 sincos 表、DQ 限幅
│   │   ├── foc_svpwm.{c,h}        min-max 零序注入调制
│   │   ├── foc_encoder.{c,h}      角度链、多圈展开、二阶巴特沃斯速度、读失败外推
│   │   ├── foc_vernier.{c,h}      30/31 齿游标绝对位置解算
│   │   └── foc_current.{c,h}      d/q 电流 PI + 母线电压增益调度（pu 域）
│   └── App/
│       ├── config.h               全部配置常量（含量纲与默认值，阅读入口）
│       ├── servo.{c,h}            三模式级联控制律（外环，V2/V3 参考）
│       ├── cali.{c,h}             电角度自校准（双向圆周均值 + 探针验证）
│       └── cali_nv.{c,h}          Flash 记录布局与写后回读（硬件耦合）
└── host_test/                 ← 本工程新写的离线验证（GPLv3，因链接上游源码）
    ├── README.md
    ├── test_foc_algorithm.c       变换可逆性、限幅边界、wrap 边界、游标解码、SVPWM
    ├── stubs/encoder.h            仅为脱离硬件编译而设的桩
    └── build_and_run.ps1          一键编译并运行（MinGW gcc，无需 ARM 工具链）
```

**未复制的内容及原因**：`main.c`/`comm.{c,h}`/`node.{c,h}`（CAN 协议与节点管理，非 FOC 算法）；
`Hw/**`（`board/adc/pwm/can/encoder/irq/debug` + SEGGER RTT，纯硬件层）；`Drivers/**`（GD32 SPL 与 CMSIS）。
按需求"不需要关心它如何控制硬件"，这些都不在范围内。若后续需要看硬件耦合点，
`servo.c`/`cali.c` 里的 `Adc_*`/`Pwm_*`/`Encoder*`/`Can_*` 调用即为全部接口面。
