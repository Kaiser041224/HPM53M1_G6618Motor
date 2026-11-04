# G6618 电机控制协议 v0.1.1

| 项目 | 内容 |
| :--- | :--- |
| 适用对象 | HPM53M1 + G6618 电机（50:1 减速器） |
| 适用固件 | `HPM53M1_G6618Motor`（2026-11-03 起） |
| 物理层 | 经典 CAN（非 FD），默认 **1 Mbps**，**11 位标准帧** |
| 字节序 | MIT 为手册大端位域；其余多字节数值为**小端**（float32 IEEE754 / uint16） |
| 坐标 | **输出轴（减速器后）** 用户坐标，正方向由 `software.motion.output_direction` 定义 |
| 反馈 | **问询式**：每收到一条通过校验的本电机帧回一帧；无周期主动上报 |
| 兼容性 | 控制帧载荷兼容 DM-J8009P-2EC 手册；**反馈 T 为本电机扩展量程 ±200 Nm**（§1.4） |

---

## 1. 协议概览

### 1.1 角色

- **主机**：PC / MCU / miniPC，发起控制帧。
- **电机**：本固件，执行控制并返回反馈帧。
- 总线仲裁：所有帧 ID ≤ `0x19F`，低于常见的 `0x2xx/0x7xx` 主机报文。

### 1.2 ID 分配

```text
id = 0x100 | (type << 4) | node          // 0x110 .. 0x18F
```

| type | 帧 ID | 方向 | 含义 |
| :--- | :--- | :--- | :--- |
| 1 | `0x11N` | 主机→电机 | MIT 控制（8B） |
| 2 | `0x12N` | 主机→电机 | 位置速度模式（8B） |
| 3 | `0x13N` | 主机→电机 | 速度模式（4~8B） |
| 4 | `0x14N` | 主机→电机 | 力位混控模式（8B） |
| 5 | `0x15N` | 主机→电机 | 使能（magic `0xFC`） |
| 6 | `0x16N` | 主机→电机 | 失能（magic `0xFD`） |
| 7 | `0x17N` | 主机→电机 | 清故障（magic `0xFE`） |
| 8 | `0x18N` | 电机→主机 | 反馈（8B） |

`N` = 节点号（0..7，见 §2）。type 0 与 9..15 保留，不匹配。

### 1.3 通用约定

- 控制帧的时间语义：**最后一条有效帧持续生效**；主机应按自己的控制周期重发（建议 1 kHz）。
- 运动命令被拒绝时**不返回错误码**；主机以“反馈是否跟随”判断，拒绝原因见终端 `can` 的 `rej`。
- 反馈在命令执行后立即组帧发送，因此运动类命令的**首帧反馈**通常仍是执行瞬间的旧状态；
  本文 §10 的“预期反馈”给的是**稳态**值。
- 必须有第二节点/适配器提供 ACK；单节点无对端时发送失败计数会增长，属正常。

### 1.4 与 DM 主机的兼容性

- **控制帧**（MIT / 位置速度 / 速度 / 力位混控 / 使能 / 失能 / 清故障）的 ID 与载荷格式
  兼容 DM 手册，DM 主机可以直接发。
- **反馈 T 的量程是本电机扩展的 ±200 Nm**，不是 DM 的 ±10 Nm。DM 主机读取反馈转矩时
  必须把 `T_MAX` 改成 200，否则会把 100 Nm 读成约 5 Nm。
- **MIT 的 `t_ff` 仍保持 DM 的 ±10 Nm**，因此 DM 主机的控制输入不受影响。
- 位置/速度反馈仍用 DM 的 ±12.5 rad / ±30 rad/s。

---

## 2. 节点号（DIP）

上电从 CANID 电阻网络读取一次，**ID = 拨码二进制** `S1<<2 | S2<<1 | S3`：

| 拨码 S1 S2 S3 | 节点号 | 反馈 ID |
| :--- | :--- | :--- |
| 000 | 0 | `0x180` |
| 001 | 1 | `0x181` |
| 010 | 2 | `0x182` |
| 011 | 3 | `0x183` |
| 100 | 4 | `0x184` |
| 101 | 5 | `0x185` |
| 110 | 6 | `0x186` |
| 111 | 7 | `0x187` |

- 运行期固定；换档需重新上电。
- 解码失败时回退 `software.can.can_id`（YAML，默认 1）。
- 终端 `can id` 可查看上电快照（`dip` / `raw` / `spread` / ADC 槽状态）。

---

## 3. 状态帧

| 功能 | ID | DLC | D0 | 行为 |
| :--- | :--- | :--- | :--- | :--- |
| 使能 | `0x15N` | ≥1 | `0xFC` | 使能 FOC；失败只计数，不置故障 |
| 失能 | `0x16N` | ≥1 | `0xFD` | 受控软停（电流归零 → 减速 → 关栅极供电） |
| 清故障 | `0x17N` | ≥1 | `0xFE` | 仅 FOC OFF 时清锁存；FAULT 态需先失能 |

- D1..D7 可为 0；建议 DLC=8。
- **magic 错误 → 丢帧，不回帧**（只计 `bad`）。
- 使能前置条件：已标定、无 STOP/TRIP 锁存、ADC/编码器有效、`cal encoder`/`motion cal` 完成。
- 正常停机顺序固定为“先关 +12V 栅极供电、等待 20 ms、再停 PWM”，`0x16N` 走该路径。

**TRIP 后的恢复顺序**：

```text
0x16N（FAULT → OFF） → 0x17N（清锁存） → 0x15N（重新使能）
```

---

## 4. 控制帧

所有控制帧要求 FOC 处于 **READY / RUN**；否则丢帧并计数，反馈 ERR 保持当前状态。

### 4.1 MIT 模式（`0x11N`，8 字节）

线缆定标：`P_MAX=12.5 rad`、`V_MAX=30 rad/s`、`T_MAX=10 Nm`（MIT t_ff）、`KP_MAX=500`、`KD_MAX=5`。
反馈 T 使用独立量程 `T_FB_MAX=200 Nm`（见 §5）。

| 位 | 字段 |
| :--- | :--- |
| D0<<8 \| D1 | `p_des[15:0]` |
| D2<<4 \| D3>>4 | `v_des[11:0]` |
| (D3&0x0F)<<8 \| D4 | `kp[11:0]` |
| D5<<4 \| D6>>4 | `kd[11:0]` |
| (D6&0x0F)<<8 \| D7 | `t_ff[11:0]` |

解码：

```text
p_des = raw/65535 × 25 − 12.5          [rad]    输出轴
v_des = raw/4095  × 60 − 30            [rad/s]  输出轴
kp    = raw/4095  × 500                 [Nm/rad]
kd    = raw/4095  × 5                   [Nm/(rad/s)]
t_ff  = raw/4095  × 20 − 10             [Nm]
```

行为：

- `p_des` 映射到**距当前连续角最近的一圈**，不是绝对多圈坐标。
- 入口与每拍都要求 `|p_des − 当前连续角| ≤ 90°`（`mit.pos_err_max_deg`）；超出即拒绝。
- `kp` / `kd` 超过本电机上限（当前 **100 / 5.0**）会被**钳到上限**，不拒绝（CAN 入口）；
  终端 `motion mit` 对超限直接拒绝。
  > `kp_max` / `kd_max` 是 **MIT 输入增益的钳位**，不是输出转矩上限。
  > 输出转矩另受 `pos_err_max_deg=90°`、`motion_iq_max_a=30 A`、动态 boost
  > （24.3 A 连续 / 30 A 2 s）与 FOC 2-norm 限幅约束：
  > 24.3 A→约 142 Nm，30 A→约 175.5 Nm。
- 增益有斜坡：`kp` 以 200 (Nm/rad)/s 逼近，`kd` 以 10 (Nm/(rad/s))/s 逼近
  （`200 × kd_max/kp_max`）。`kp=100`、`kd=5` 约 0.5 s 到满值；
  阶跃增益的前 0.5 s 力矩偏软属正常。
- `t_ff` 受力矩上限约束（当前比例 1.0）。
- MIT 默认开启（`software.control.mit.enable: true`）；如需禁用：
  `param set software.control.mit.enable 0`。
- 输出力矩 `τ = kp_eff·e_p + kd_eff·(v_des − v) + t_ff`，无积分。

### 4.2 位置速度模式（`0x12N`，8 字节）

| 字节 | 字段 |
| :--- | :--- |
| D0..D3 | `p_des`，float32 小端 [rad]（输出轴单圈） |
| D4..D7 | `v_des`，float32 小端 [rad/s]（本次最大速度，取绝对值） |

- `p_des` 按 `wrap_2pi()` 折到 `[0, 2π)`，走**最短路径**。
- `v_des` 钳到 `[0.01, 30] rad/s`；再与 YAML 轨迹上限（30 rad/s）取小。
- 位置轨迹要求 `trajectory_vel_limit/accel_limit` 均 >0（当前 30 / 60），否则命令被拒。
- 与 MIT 不同：这是**单圈绝对位置**，不是最近圈连续坐标。

### 4.3 速度模式（`0x13N`，4~8 字节）

| 字节 | 字段 |
| :--- | :--- |
| D0..D3 | `v_des`，float32 小端 [rad/s]（输出轴） |

- DLC 接受 4..8，只取前 4 字节。
- 受速度环增益、`motion_speed_limit_rad_s`、电流/boost 限幅约束。

### 4.4 力位混控模式（`0x14N`，8 字节）

| 字节 | 字段 |
| :--- | :--- |
| D0..D3 | `p_des`，float32 小端 [rad]（输出轴单圈，最短路径） |
| D4..D5 | `v_des`，uint16 小端，×100 → 0..100 rad/s（>10000 钳 100） |
| D6..D7 | `i_des`，uint16 小端，×10000 → 0..1 标幺（>10000 钳 1.0） |

- 电流上限 = `i_des × motor.i_peak_10s_a`（24.3 A），再与 `motion_iq_max_a`（30 A）
  和动态 boost 限幅取小。
- `v_des=0` 按 0.01 rad/s 处理（避免“永不动”）。

---

## 5. 反馈帧（`0x18N`，8 字节）

| 字节 | 内容 |
| :--- | :--- |
| D0 | `(ERR << 4) \| (node & 0x0F)` |
| D1 | POS[15:8] |
| D2 | POS[7:0] |
| D3 | VEL[11:4] |
| D4 | `(VEL[3:0] << 4) \| T[11:8]` |
| D5 | T[7:0] |
| D6 | T_MOS [℃] |
| D7 | T_Rotor [℃] |

解码：

```text
pos_rad   = POS_raw/65535 × 25 − 12.5     // 输出轴单圈 [-π, π) 落在 ±12.5 码域内
vel_rad_s = VEL_raw/4095  × 60 − 30
torque_nm = T_raw/4095    × 400 − 200     // kt × 实测 iq × 减速比，非转矩传感器
```

- POS 分辨率 0.38 mrad/LSB；本电机只用 `[-π, π)` 段（约 26% 码值）。
- 转矩是按 `kt × 实测 iq × reducer` 的估算值，按 **±200 Nm** 编码（0.098 Nm/LSB）；
  这是本电机的反馈量程，**与 DM 的 ±10 Nm 不同**（见 §1.4）。
- 温度：`0xFF` = 无效/未接；不要把 0xFF 当温度值。
  NTC0 = MOS/功率级，NTC1 = 定子（当前硬件开路，恒为 0xFF）。
- 未标定或反馈无效时 POS/VEL/T 置 0，ERR=F。

### 5.1 ERR 码

| ERR | 含义 | 本电机来源 |
| :--- | :--- | :--- |
| 0 | 失能 / 无故障 | FOC OFF 且无锁存 |
| 1 | 使能正常 | FOC READY / RUN |
| 8 | 过压 | `VBUS_OV` |
| 9 | 欠压 | `VBUS_UV` |
| A | 过流 | `OC_FAST_*` / `OC_SLOW_*` / `OC_TRIP` |
| B | MOS 过温 | **未接入**（无对应故障码） |
| C | 线圈过温 | **未接入**（NTC1 当前开路） |
| D | 通讯丢失 | `COMM_TIMEOUT` |
| E | 过载 | 预留 |
| F | 其他 | ADC/编码器/反馈无效 |

多故障优先级：**A > 8 > 9 > D > F > 1/0**（B/C 未接入）。

---

## 6. 运行时行为

### 6.1 模式切换

- 模式由帧 ID 隐式决定，没有模式寄存器。
- 运动进行中收到**不同运动模式**的帧：
  1. 调用一次“释放输出”（零力矩，**保持 FOC 使能**）；
  2. 丢弃本帧并计数（终端 `can` 的 `switch`）；
  3. 下一帧起接受新模式。
- `0x12N`（位置速度）与 `0x14N`（力位混控）同属位置模式，互相切换**不**触发释放。
- 要彻底停机用 `0x16N`，不要依赖模式切换。

### 6.2 命令拒绝

以下情况丢帧并计数（`can` 的 `rej`），反馈 ERR 不变，主机应以“位置/速度是否跟随”判断：

- FOC 非 READY/RUN；
- 未标定 / 连续角不可信 / 反馈过期；
- 位置模式未配置轨迹或增益；
- MIT 未启用、位置误差入口超过 `mit.pos_err_max_deg`（`kp/kd` 在 CAN 入口已钳位，
  不会因增益超限被拒）；
- 行程保护（单次 3600° / 绝对 36000° / 可选软限位）。

### 6.3 通信看门狗

- 首个有效控制帧后 arm；超时 `100 ms`（`software.comm.comm_timeout_ms`）。
- 喂狗双源：CAN 控制帧（RX 中断）+ USB 终端输入；任一活跃即保持。
- 超时置 `COMM_TIMEOUT`；出厂 `fault.actions_enable=false` **只告警**，
  需要停机时 `fault protect on` 或改 YAML 后重烧。
- 终端 `can` 显示 `wd=armed/susp/fired`。

### 6.4 每拍预算

- 1 kHz 服务：每拍最多处理 **8 帧**、最多回 **8 帧**。
- 正常 1 kHz 控制 = 1 帧/ms、1 反馈/ms。
- 总线必须有 ACK；单节点无对端时 `tx_fail`/`tec` 增长、`ep=1` 属正常。

---

## 7. 线缆定标与本电机限幅

| 量 | 线缆定标 | 本电机实际（当前参数） |
| :--- | :--- | :--- |
| 位置 | ±12.5 rad | 单圈 `[-π, π)` |
| 速度 | ±30 rad/s | `motion_speed_limit_rad_s=0`（不限）；FOC 内部限速 0（关闭） |
| 转矩 | MIT t_ff ±10 Nm；反馈 T ±200 Nm | `motion_iq_max_a=30 A`、连续 24.3 A、boost 30 A/2 s |
| MIT Kp | 0..500 | ≤100（输入增益钳位；超出 CAN 静默钳位、终端拒绝） |
| MIT Kd | 0..5 | ≤5.0（输入增益钳位；线缆满量程） |
| MIT 位置误差 | — | ≤90°（与电流限幅共同决定输出转矩） |
| 位置轨迹 | — | 30 rad/s、60 rad/s² |

转矩估算：`T_out = kt(0.117 Nm/A) × iq × reducer(50)`；1 A iq ≈ 5.85 Nm。

> **MIT 限幅口径**：`kp_max/kd_max` 只限制输入增益，不是输出转矩上限。
> 输出转矩由 `pos_err_max_deg`（90°）+ 电流限幅（24.3 A 连续 / 30 A boost）共同决定。
>
> **参数持久化**：`mit.*` 是 live 参数，`param set` 立即生效；
> `param save`（FOC OFF）写入 flash SPRM。`storage: keep` 时启动优先使用 flash 值，
> YAML 只在无 SPRM 记录时生效——所以改完必须 `param save`，否则重启会回到旧值。
>
> **可设范围**：`kp_max` 的生成器上限是 **100**（`param set` 超过会拒绝）；
> `kd_max` 的生成器上限是 20，但 CAN 的 `kd` 线缆满量程是 **5**——
> 把 `kd_max` 设到 5 以上只对终端 `motion mit` 有意义，对 CAN 输入不会生效。

---

## 8. 终端诊断

> 终端命令的完整说明见 [usb-terminal_v0.1.1.md](usb-terminal_v0.1.1.md)。

```text
can            # node / wd / bus(tec,rec,ep) / dm rx,drop,bad,foreign,switch,rej,txf
can id         # 上电 DIP 快照（dip/raw/spread）+ ADC 槽状态
foc            # FOC 状态、限幅、vbus src
motion show    # 模式、位置、速度、iq、MIT 诊断（mit_kp_eff / mit_torque_nm…）
adc dump       # V_VBUS / NTC 温度
fault          # 故障锁存与 policy
```

---

## 9. 控制帧速查（节点 0）

> 其他节点把 ID 低 4 位改成 `N`。全部按 DLC=8 给出；magic 帧 D1..D7 可为 0。

### 状态

```text
使能              0x150：FC 00 00 00 00 00 00 00
失能              0x160：FD 00 00 00 00 00 00 00
清故障            0x170：FE 00 00 00 00 00 00 00
```

### 速度

```text
0 rad/s           0x130：00 00 00 00 00 00 00 00
+1 rad/s          0x130：00 00 80 3F 00 00 00 00
-1 rad/s          0x130：00 00 80 BF 00 00 00 00
+5 rad/s          0x130：00 00 A0 40 00 00 00 00
```

### 位置速度

```text
p=0.5 / v=1       0x120：00 00 00 3F 00 00 80 3F
p=0.5 / v=5       0x120：00 00 00 3F 00 00 A0 40
p=0   / v=5       0x120：00 00 00 00 00 00 A0 40
p=π/2 / v=2       0x120：DB 0F C9 3F 00 00 00 40
```

### 力位混控

```text
p=0.5 / v=5 / i=0.1   0x140：00 00 00 3F F4 01 E8 03
p=0.5 / v=2 / i=0.2   0x140：00 00 00 3F C8 00 D0 07
p=0.5 / v=5 / i=0     0x140：00 00 00 3F F4 01 00 00
```

### MIT

```text
零力矩（自由）            0x110：7F FF 7F F0 00 00 07 FF
纯阻尼 kp=0 kd=1.0        0x110：7F FF 7F F0 00 33 37 FF
高阻尼 kp=0 kd=2.0        0x110：7F FF 7F F0 00 66 67 FF
速度阻尼 v=+1 kd=1.0      0x110：7F FF 84 30 00 33 37 FF
柔顺保持 kp=2 kd=0.2      0x110：7F FF 7F F0 10 0A 37 FF
保持 p=0 kp=5 kd=0.5      0x110：7F FF 7F F0 28 19 97 FF
保持 p=0 kp=10 kd=0.5     0x110：7F FF 7F F0 51 19 97 FF
保持 p=0.5 kp=10 kd=0.5   0x110：85 1E 7F F0 51 19 97 FF
移到 0.2 kp=10 kd=0.5     0x110：82 0B 7F F0 51 19 97 FF
移到 0.5 kp=20 kd=1.0     0x110：85 1E 7F F0 A3 33 37 FF
移到 1.0 kp=30 kd=1.5     0x110：8A 3C 7F F0 F5 4C C7 FF
移到 1.4 (80°) kp=20 kd=1 0x110：8E 55 7F F0 A3 33 37 FF
移到 -0.5 kp=10 kd=0.5    0x110：7A E0 7F F0 51 19 97 FF
移到 -1.0 kp=20 kd=1.0    0x110：75 C2 7F F0 A3 33 37 FF
保持 0 +0.5 Nm           0x110：7F FF 7F F0 51 19 98 65
保持 0 -0.5 Nm           0x110：7F FF 7F F0 51 19 97 99
保持 0 +1.0 Nm           0x110：7F FF 7F F0 51 19 98 CC
保持 0 -1.0 Nm           0x110：7F FF 7F F0 51 19 97 32
高刚度 kp=50 kd=2.0      0x110：7F FF 7F F1 99 66 67 FF
```

---

## 10. 示例：控制帧与预期反馈帧

### 10.1 反馈基线

以下示例假设：

- 节点 0，已使能、已标定，初始静止在 **输出轴 0 rad**；
- MOS 温度 40 ℃，`NTC1` 开路 → `0xFF`；
- 无故障。

对应基线反馈：

```text
0x180：10 7F FF 7F F7 FF 28 FF
       │  └──┬──┘ └─┬─┘ └┬┘ └┬┘
       │   POS=0   VEL=0  T=0  T_MOS=40 / T_Rotor=0xFF
       └ ERR=1, node=0
```

> 运动类命令的**首帧**反馈通常是命令执行瞬间的旧状态；表中给的是**稳态**值
> （持续 1 kHz 发送、运动完成后的典型反馈）。

### 10.2 逐条示例

| # | 发送（节点 0） | 预期反馈（稳态） | 说明 |
| :--- | :--- | :--- | :--- |
| 1 | `0x150：FC 00 00 00 00 00 00 00` | `0x180：10 7F FF 7F F7 FF 28 FF` | 使能成功，ERR=1，静止 0 |
| 2 | `0x130：00 00 80 3F 00 00 00 00` | `0x180：10 81 05 84 37 FF 28 FF` | 速度 +1 rad/s，运动到 pos≈0.1 rad 时 |
| 3 | `0x130：00 00 00 00 00 00 00 00` | `0x180：10 83 11 7F F7 FF 28 FF` | 速度 0，停在 pos≈0.3 rad |
| 4 | `0x120：00 00 00 3F 00 00 A0 40` | `0x180：10 85 1E 7F F7 FF 28 FF` | 位置 0.5 rad，到达后 vel≈0 |
| 5 | `0x140：00 00 00 3F F4 01 E8 03` | `0x180：10 85 1E 7F F7 FF 28 FF` | 力位混控 0.5 rad；空载时 T≈0，带载时 T 随负载 |
| 6 | `0x110：7F FF 7F F0 51 19 97 FF` | `0x180：10 7F FF 7F F7 FF 28 FF` | MIT 保持 0（kp=10 kd=0.5），静止 |
| 7 | `0x110：85 1E 7F F0 A3 33 37 FF` | `0x180：10 85 1E 7F F7 FF 28 FF` | MIT 移到 0.5 rad（kp=20 kd=1.0），到达后 |
| 8 | `0x110：7F FF 7F F0 51 19 98 65` | `0x180：10 80 82 7F F8 04 28 FF` | MIT 保持 0 +0.5 Nm；稳态位置误差 ≈0.05 rad，T≈0.5 Nm |
| 9 | `0x160：FD 00 00 00 00 00 00 00` | `0x180：00 85 1E 7F F7 FF 28 FF` | 软停完成，FOC OFF，停在 0.5 rad |
| 10 | `0x170：FE 00 00 00 00 00 00 00` | `0x180：00 85 1E 7F F7 FF 28 FF` | OFF 且无故障，清锁存后状态不变 |
| 11 | 故障注入（过流） | `0x180：A0 7F FF 7F F7 FF 28 FF` | ERR=A；位置为故障时所在位置 |

说明：

- 第 2 条：`pos` 随运行时间变化，`81 05` 只是“pos≈0.1 rad”时的一个采样；
  速度稳态为 `84`（≈1.0 rad/s）。
- 第 5 条：力位混控的转矩反馈取决于实际负载；空载/水平台架通常接近 0。
- 第 8 条：`kp=10`、`t_ff=0.5 Nm` 时稳态误差 `e=t_ff/kp=0.05 rad`；
  `80 82` 对应 pos≈0.05 rad，`04` 对应 T≈0.5 Nm（反馈 T 量程 ±200）。
- 第 9 条：软停需要数百 ms；主机需持续发帧才能看到 ERR 从 1 变 0。
- 第 11 条：故障后位置/速度/转矩仍按当前快照上报；若反馈无效则置 0 且 ERR=F。

### 10.3 模式切换示例

```text
主机正在以 0x130 发速度 +1 rad/s
→ 改发 0x120：00 00 00 3F 00 00 A0 40
```

- 第一帧：电机调用 `app_motion_release_output()`，**丢弃该位置帧**，反馈仍是速度模式的状态；
  终端 `can` 的 `switch` +1。
- 第二帧起：进入位置模式，反馈位置开始向 0.5 rad 收敛。

---

## 11. 编解码参考（C++17）

以下代码只依赖标准库，字节拼接不依赖主机字节序，主机侧可直接复用。

```cpp
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dm_can {

constexpr float kPMax   = 12.5f;
constexpr float kVMax   = 30.0f;
constexpr float kTFFMax = 10.0f;  // MIT t_ff
constexpr float kTFbMax = 200.0f; // 反馈 T
constexpr float kKpMax  = 500.0f;
constexpr float kKdMax  = 5.0f;

using Bytes8 = std::array<std::uint8_t, 8>;

inline void put_f32_le(Bytes8& out, std::size_t off, float v) {
    std::uint32_t u;
    std::memcpy(&u, &v, sizeof(u));
    out[off + 0] = static_cast<std::uint8_t>(u);
    out[off + 1] = static_cast<std::uint8_t>(u >> 8);
    out[off + 2] = static_cast<std::uint8_t>(u >> 16);
    out[off + 3] = static_cast<std::uint8_t>(u >> 24);
}

inline float get_f32_le(const Bytes8& in, std::size_t off) {
    const std::uint32_t u = static_cast<std::uint32_t>(in[off])
                          | (static_cast<std::uint32_t>(in[off + 1]) << 8)
                          | (static_cast<std::uint32_t>(in[off + 2]) << 16)
                          | (static_cast<std::uint32_t>(in[off + 3]) << 24);
    float v;
    std::memcpy(&v, &u, sizeof(v));
    return v;
}

inline void put_u16_le(Bytes8& out, std::size_t off, std::uint16_t v) {
    out[off]     = static_cast<std::uint8_t>(v);
    out[off + 1] = static_cast<std::uint8_t>(v >> 8);
}

inline std::uint32_t float_to_uint(float x, float lo, float hi, unsigned bits) {
    const std::uint32_t max_raw = (1u << bits) - 1u;
    if (!(x > lo)) return 0;          // 含 NaN
    if (x >= hi) return max_raw;
    return static_cast<std::uint32_t>((x - lo) * static_cast<float>(max_raw) / (hi - lo));
}

inline float uint_to_float(std::uint32_t raw, float lo, float hi, unsigned bits) {
    const std::uint32_t max_raw = (1u << bits) - 1u;
    return static_cast<float>(raw) * (hi - lo) / static_cast<float>(max_raw) + lo;
}

// MIT：p [rad]、v [rad/s]、kp [Nm/rad]、kd [Nm/(rad/s)]、t_ff [Nm]
inline Bytes8 mit_encode(float p, float v, float kp, float kd, float t_ff) {
    const auto pr = float_to_uint(p, -kPMax, kPMax, 16);
    const auto vr = float_to_uint(v, -kVMax, kVMax, 12);
    const auto kr = float_to_uint(kp, 0.0f, kKpMax, 12);
    const auto dr = float_to_uint(kd, 0.0f, kKdMax, 12);
    const auto tr = float_to_uint(t_ff, -kTFFMax, kTFFMax, 12);

    return {
        static_cast<std::uint8_t>(pr >> 8),
        static_cast<std::uint8_t>(pr),
        static_cast<std::uint8_t>(vr >> 4),
        static_cast<std::uint8_t>(((vr & 0x0Fu) << 4) | (kr >> 8)),
        static_cast<std::uint8_t>(kr),
        static_cast<std::uint8_t>(dr >> 4),
        static_cast<std::uint8_t>(((dr & 0x0Fu) << 4) | (tr >> 8)),
        static_cast<std::uint8_t>(tr),
    };
}

// 反馈编码（主机侧自测/模拟电机用）
inline Bytes8 feedback_encode(std::uint8_t err, std::uint8_t node, float pos, float vel,
                              float torque, std::uint8_t t_mos = 0xFF,
                              std::uint8_t t_rotor = 0xFF) {
    const auto pr = float_to_uint(pos, -kPMax, kPMax, 16);
    const auto vr = float_to_uint(vel, -kVMax, kVMax, 12);
    const auto tr = float_to_uint(torque, -kTFbMax, kTFbMax, 12);

    return {
        static_cast<std::uint8_t>(((err & 0x0Fu) << 4) | (node & 0x0Fu)),
        static_cast<std::uint8_t>(pr >> 8),
        static_cast<std::uint8_t>(pr),
        static_cast<std::uint8_t>(vr >> 4),
        static_cast<std::uint8_t>(((vr & 0x0Fu) << 4) | (tr >> 8)),
        static_cast<std::uint8_t>(tr),
        t_mos,
        t_rotor,
    };
}

struct Feedback {
    std::uint8_t err;
    float pos_rad;
    float vel_rad_s;
    float torque_nm;
    std::uint8_t t_mos_c;
    std::uint8_t t_rotor_c;
};

inline Feedback feedback_decode(const Bytes8& d) {
    const std::uint32_t pr = (static_cast<std::uint32_t>(d[1]) << 8) | d[2];
    const std::uint32_t vr = (static_cast<std::uint32_t>(d[3]) << 4) | (d[4] >> 4);
    const std::uint32_t tr = (static_cast<std::uint32_t>(d[4] & 0x0Fu) << 8) | d[5];

    return {
        static_cast<std::uint8_t>(d[0] >> 4),
        uint_to_float(pr, -kPMax, kPMax, 16),
        uint_to_float(vr, -kVMax, kVMax, 12),
        uint_to_float(tr, -kTFbMax, kTFbMax, 12),
        d[6],
        d[7],
    };
}

// 位置速度：p [rad] + v_max [rad/s]
inline Bytes8 pos_vel_encode(float p, float v) {
    Bytes8 out{};
    put_f32_le(out, 0, p);
    put_f32_le(out, 4, v);
    return out;
}

struct PosVel {
    float p_rad;
    float v_rad_s;
};

inline PosVel pos_vel_decode(const Bytes8& d) {
    return {get_f32_le(d, 0), get_f32_le(d, 4)};
}

// 速度：v [rad/s]（只占 D0..D3）
inline Bytes8 velocity_encode(float v) {
    Bytes8 out{};
    put_f32_le(out, 0, v);
    return out;
}

// 力位混控：p [rad] + v [rad/s] + i_ratio [0,1]
inline Bytes8 force_pos_encode(float p, float v_rad_s, float i_ratio) {
    Bytes8 out{};
    put_f32_le(out, 0, p);
    put_u16_le(out, 4, static_cast<std::uint16_t>(v_rad_s * 100.0f));
    put_u16_le(out, 6, static_cast<std::uint16_t>(i_ratio * 10000.0f));
    return out;
}

} // namespace dm_can
```

### 11.1 使用示例

> 示意代码：`can_write()` / `printf()` 省略，实际调用换成你的 CAN 驱动接口。

```cpp
const std::uint8_t node = 0;
dm_can::Bytes8 data;

// 使能：0x150，D0=0xFC
data = {};
data[0] = 0xFC;
// can_write(0x150u | node, data);

// 速度 +1 rad/s：0x130
// can_write(0x130u | node, dm_can::velocity_encode(1.0f));

// 位置 0.5 rad / 最大 5 rad/s：0x120
// can_write(0x120u | node, dm_can::pos_vel_encode(0.5f, 5.0f));

// MIT：p=0.5, v=0, kp=20, kd=1.0, t_ff=0：0x110
// can_write(0x110u | node, dm_can::mit_encode(0.5f, 0.0f, 20.0f, 1.0f, 0.0f));

// 反馈解码（0x180 | node 收到的 8 字节）
// dm_can::Feedback fb = dm_can::feedback_decode(rx);
// printf("err=%X pos=%.3f vel=%.3f tq=%.3f Tmos=%u\n",
//        fb.err, fb.pos_rad, fb.vel_rad_s, fb.torque_nm, fb.t_mos_c);
```

> MIT `t_ff` 用 `kTFFMax=10`，反馈 T 用 `kTFbMax=200`。

---

## 12. 已知限制

1. **过温 ERR B/C 未接入**：固件没有 OT 故障码与阈值，不虚构上报。
2. **MIT 默认开启**：如需禁用，`param set software.control.mit.enable 0`。
3. **位置模式必须有梯形轨迹**：`trajectory_vel_limit` 与 `trajectory_accel_limit` 均 >0。
4. **反馈是问询式**：不发帧就没有反馈；主机需按自己的控制周期发送。
5. **必须有 ACK**：单节点无对端时发送失败计数会增长。
6. **MIT 位置误差 90°**：持续流式发送时目标必须始终在当前位置 90° 内。
7. **多圈绝对位置不在 CAN 上暴露**：CAN 只表达输出轴单圈；多圈/连续角走 USB 终端。
8. **CAN 参数读写/标定/升级不做**：这些走 USB 终端。
9. **输出编码器持续故障的代价**：位置反馈需要输出 pair；持续故障会使反馈 ERR=F，
   并可能拒绝运动命令。
10. **反馈 T 刻度 ±200 Nm**：与 DM 的 ±10 Nm 不同；DM 主机读取反馈转矩需改常量，
    控制输入（MIT t_ff 仍 ±10 Nm）不受影响。
