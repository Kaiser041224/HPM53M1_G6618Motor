# DM 兼容 CAN 控制协议设计（运行时通道）

- 状态：**已实施**（2026-11-03：编解码 / 策略 / 1kHz 服务 / 运动层支撑）
- 实施决议：位置/速度/KP/KD 沿用 DM 默认（12.5/30/500/5）；MIT t_ff 保持 DM 的 ±10 Nm，
  **反馈 T 使用本电机全量程 ±200 Nm**（DM 主机读反馈 T 需改常量，控制输入不受影响）；
  MIT 能力对齐：`kp_max=100`、`kd_max=5.0`，电流限幅成为唯一瓶颈；
  MIT 位置按最近圈映射；`i_des` 分母用 `motor.i_peak_10s_a`；转矩反馈用实测 iq；
  B/C（过温）暂未接入（无对应故障码，阈值待定）；`COMM_TIMEOUT` 出厂仍只告警
  （`fault.actions_enable=false`）。
- 日期：2026-11-02
- 参考：`Doc/datasheet/DM-J8009P-2EC 减速电机说明书 V1.3 2026-09-10.pdf`
- 范围：CAN 运行时控制报文与反馈报文；USB / UART 继续承担调试与参数整定

---

## 1. 目标与非目标

### 1.1 目标

1. 在总线上复现 DM-J8009P-2EC 的**四类控制帧**与**统一反馈帧**，使其上位机（MCU / miniPC）按 DM 手册的格式即可驱动本机。
2. 保留本工程已有的安全链：使能门控、故障锁存、通信看门狗、软停机。
3. 协议层保持纯函数，可在主机上做编解码回归。

### 1.2 非目标（明确不做）

- 不通过 CAN 做参数读写、标定、波形、固件升级（这些走 USB / UART）。
- 不通过 CAN 设置 CAN ID / 波特率 / 保存参数（由 YAML + USB 终端完成）。
- 不暴露多圈绝对位置（与 DM 一致，CAN 只表达输出轴单圈）。
- 不实现 DM 手册未给出的「模式寄存器」；模式由帧 ID 隐式决定。

### 1.3 关键取舍

| 取舍 | 决定 | 理由 |
| :--- | :--- | :--- |
| 与现有 CANSimple 协议的关系 | **整体替换**，不并存 | 两者 ID 空间重叠（DM 的 `CAN_ID` 会与 `(node<<5)|cmd` 冲突）；并存只会制造歧义 |
| 反馈触发方式 | **问询式**：每收到一条有效控制帧回一帧 | 与手册一致；1kHz 控制即 1kHz 反馈 |
| 位置语义 | 输出轴单圈，`[-π, π)` | 与手册「上电后位置限定在 [-π,π]」一致 |
| 使能/失能 | 扩展帧，见 §4 | 手册未给出，但运行必需 |
| 帧 ID | `0x1 | type | node`，全部 ≤ `0x19F` | 低 ID 仲裁优先；低 4 位直接是节点号（§2.2） |
| 节点号 | 上电从 CANID 电阻网络读一次后固定 | 同一固件区分多台；YAML 仅兜底（§2.3） |

---

## 2. 总线与 ID 分配

### 2.1 基本参数

- 标准帧（11 位 ID），不使用扩展帧。
- 默认 1 Mbps（`software.can.baudrate`）。
- 小端字节序（与 MCU 一致；编解码按字节组装，不做指针强转）。

### 2.2 ID 结构（本工程优化，非 DM 原版）

所有帧 ID 统一为 `0x1XX`，其中**低 4 位 = 节点号**，中间 4 位 = 帧类型：

```text
id = 0x100 | (type << 4) | node_id        // 0x110 .. 0x19F
```

| type | 帧 ID | 方向 | 含义 |
| :--- | :--- | :--- | :--- |
| 1 | `0x11X` | 主机→电机 | MIT 控制 |
| 2 | `0x12X` | 主机→电机 | 位置速度模式 |
| 3 | `0x13X` | 主机→电机 | 速度模式 |
| 4 | `0x14X` | 主机→电机 | 力位混控模式 |
| 5 | `0x15X` | 主机→电机 | 使能（扩展） |
| 6 | `0x16X` | 主机→电机 | 失能（扩展） |
| 7 | `0x17X` | 主机→电机 | 清故障（扩展） |
| 8 | `0x18X` | 电机→主机 | 反馈 |
| 9 | `0x19X` | — | 保留 |

- `X = node_id`，协议上占 4 位（0..15）。**当前板载 DIP 按原理图只能编码 0..7**（见 §2.3）；节点号 **0..7 均可用**（000 = 节点 0），8..15 保留给以后改板或外部写入。
- **仲裁**：所有 ID ≤ 0x19F，数值低于常见的 `0x2xx / 0x7xx` 报文；总线拥挤时电机帧优先，不会被高 ID 主机报文长期压制。
- 匹配规则：

```text
(id >> 8) == 0x1                     → 属于本 ID 方案
type    = (id >> 4) & 0x0F           → 帧类型
node_id = id & 0x0F                  → 目标节点
```

- 同一 `node_id` 不会同时收两种类型：类型由中间位唯一决定。
- 类型 `0` 与 `A..F` 未定义：匹配到本节点也一律丢弃并计数（不回复）。
- **与 DM 原版的差异**：DM 用 `CAN_ID / 0x100+ID / 0x200+ID / 0x300+ID`，ID 可能到 0x3FF+；本方案是用户要求的 ID 优化。**载荷格式仍按手册**，但上位机必须按本表的 ID 发帧，不能直接复用达妙原版主机的 ID 生成代码。

### 2.3 节点号来源（上电读取一次，固定）

- 上电初始化阶段从 CANID 电阻网络（ADC1_IN15 / PB00，`ADC_CH_V_CANID`）读**一次**原始码；
- 用阈值表解码为 `0..7`（原理图给出 3 个 ID 位；实测见 §2.3.1）；
- 解码成功后写入 RAM（`s_node_id`），运行期不再采样、不再改变；
- 解码失败 / 阈值表未配置 → 回退 `software.can.can_id`（YAML，默认 1），启动日志与 `can` 诊断打印 `WARN` 及原始码；
- 读取完成后把 CANID 槽从 ADC1 SEQ 队列移除（见 §2.4）。

#### 2.3.1 原理图读图结果（2026-11-02，`Doc/datasheet/HPM-FOC_G6618.pdf` 第 2 页）

网络（根图，不在 Analog Signal Processing 页）：

```text
+3.3V ── R74 15K ──┐
       ── R75 10K ──┤ SW2 S1/S2/S3（左侧 1..3 公共）
       ── R76 3.3K ─┘        │
                              ├── ADC_V_CANID ── PB00 / ADC1_IN15
                              └── R78 3.3K ── GND

+3.3V ── R79 3.3K ── SW2 S4 ── RES_CTL（CAN 120Ω 终端使能）
```

- **S1 = R74 15K，S2 = R75 10K，S3 = R76 3.3K，S4 = R79 3.3K → RES_CTL**；S4 不参与 ID。
- 因此原理图只有 **8 档（3 位）**，不是 `hardware.yaml` 原先写的 16 档。实际焊接可能不同，以实测为准。
- 标称电压（16 位 ADC、3.3 V 参考，R78=3.3K）：

| S1 S2 S3 | 闭合上拉 | V | raw16（标称） |
| :--- | :--- | :--- | :--- |
| 000 | 无 | 0.000 | 0 |
| 100 | 15K | 0.595 | 11818 |
| 010 | 10K | 0.819 | 16261 |
| 110 | 15K∥10K | 1.171 | 23254 |
| 001 | 3.3K | 1.650 | 32768 |
| 101 | 15K∥3.3K | 1.814 | 36015 |
| 011 | 10K∥3.3K | 1.884 | 37408 |
| 111 | 全部 | 2.006 | 39835 |

- 标称码值随 `S1 + 2·S2 + 4·S3` 单调递增；最紧的一档是 101↔011（约 70 mV / 1390 LSB），1% 电阻最坏仍有约 45 mV 间隔。
- 实测时 S4 开/关不应改变 `ADC_V_CANID`；若改变，说明装配与原理图不一致。

**2026-11-02 台架实测（16 位 raw）：**

| 拨码（按用户测试顺序） | 实测 V | 实测 raw | 反推上拉 |
| :--- | :--- | :--- | :--- |
| 000 | 0.0256 | 508 | 无（ADC 零点偏置） |
| 100 | 0.5908 | 11733 | 15K |
| 010 | 1.6733 | 33230 | 3.3K |
| 001 | 0.8205 | 16295 | 10K |
| 110 | 1.7771 | 35292 | 15K∥3.3K |
| 101 | 1.1587 | 23011 | 15K∥10K |
| 011 | 1.8767 | 37277 | 3.3K∥10K |
| 111 | 1.9989 | 39697 | 全部 |

**2026-11-02 确认：以上实测值作为最终 8 档 ID 依据。** 网络拓扑与原理图一致（上拉选择 + R78 分压），但当前装配的中间两档与原理图对调——按物理 `S1 S2 S3` 顺序，实际是 `S1=15K、S2=3.3K、S3=10K`（原理图为 `S1=15K、S2=10K、S3=3.3K`），即 R75(10K) 与 R76(3.3K) 贴反。**固件按当前装配的实测值建表，不依赖原理图位序。**

最终解码表（**ID = 拨码二进制 `S1<<2 | S2<<1 | S3`**；阈值列仍按电压低→高排序，两者不同序，固件用 level→ID 置换表）：

| 电压档位 | ID | 拨码 S1 S2 S3 | 实测 V | 实测 raw | 阈值下限（raw，`>=` 进档） |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 0 | 0 | 000 | 0.0256 | 508 | 0 |
| 1 | 4 | 100 | 0.5908 | 11733 | 6120 |
| 2 | 1 | 001 | 0.8205 | 16295 | 14014 |
| 3 | 5 | 101 | 1.1587 | 23011 | 19653 |
| 4 | 2 | 010 | 1.6733 | 33230 | 28120 |
| 5 | 6 | 110 | 1.7771 | 35292 | 34261 |
| 6 | 3 | 011 | 1.8767 | 37277 | 36284 |
| 7 | 7 | 111 | 1.9989 | 39697 | 38487 |

- 阈值取相邻两档 raw 的中点；固件用 `raw >= 阈值[n]` 递增得到**电压档位**，再查
  `s_can_id_level_to_id[] = {0,4,1,5,2,6,3,7}` 得到 ID。两张表必须同步维护。
- 最紧的间隔是电压档位 5↔6（35292↔37277，1985 raw ≈ 100 mV），16 位 ADC 足够；不要用标称值替代实测表。
- **若返修 R75/R76 使装配与原理图一致，本表失效，必须重新实测并更新。**

> 这样节点号只由板上电阻网络决定，烧录同一固件即可区分多台电机；YAML 值仅作兜底。
>
> **初始化顺序要改**：当前 `app_init()` 的步骤 5 调用 `app_can_service_init()`，步骤 11 才 `app_adc_init()`。节点号读取必须晚于 ADC，故 `app_can_service_init()` 需后移到 ADC 初始化之后（新步骤 11b）；`app_debug_can_init()`（外设自检）仍留在步骤 5。移动后，启动早期到达的 CAN 帧无人接收（无回调），属预期。

### 2.4 CANID 采样槽的移除

当前 ADC1 SEQ（30kHz、PWM1_CH10REF 同源触发）为 5 槽：

```text
[V_VBUS, NTC0, NTC1, CANID, V_VBUS]
```

上电读取 ID 后重配为 4 槽：

```text
[V_VBUS, NTC0, NTC1, V_VBUS]
```

实现要求：

- 新增 `app_adc_canid_release()`（Platform）：停止 ADC1 SEQ → 重写队列 → 重启。只改 ADC1，不碰 ADC0、TRGM 与触发相位。
- 重配前必须确认 ADC1 已产出至少一帧（读 `intf_adc_get_seq_frames()` 或等待 ≥2ms）；不满足则**保留 5 槽**、记录降级标志，节点号仍用 YAML 兜底。
- `s_slow_channels` 由 4 项降为 3 项；`APP_ADC_SLOW_SEQ_COUNT` 从编译期常量改为运行期值（或保留 5 槽静态断言 + 4 槽运行期配置）。
- **首末同通道（V_VBUS）规则保持不变**：VBUS 仍在首槽，FOC 快通道的读取时序不变。
- **重配窗口**：必须在 FOC 使能前完成（建议在 `app_init()` 内）。窗口内 vbus 快通道可能短暂读到无效值，此时桥未使能，无控制风险；重配后需确认 SEQ 帧计数继续推进，否则回退 5 槽。
- **验证红线**：AGENTS 的 ADC1 冻结条款要求改槽序后重跑 spec `2026-09-19-adc-sampling-design` §9.5.2 的 vbus 配对 A/B 与 `cal rl`。本次 5→4 槽同样适用（整帧缩短约 1.15µs）。
- 移除后 `ADC_CH_V_CANID` 不再更新：`adc dump` / `status` 显示「已固定为 N」而不是实时电压；`app_analog_signal` 不再暴露该通道的物理量。

### 2.5 多节点与仲裁

- 命令逐节点寻址（低 4 位），不存在「一条命令同时驱动多台」的广播语义。
- 多台同时反馈时按 ID 数值仲裁：节点号小者先发，其余自动重试；不会丢帧（CAN 控制器自动重发）。
- 反馈 `0x18X` 高于命令 `0x11X..0x17X`：主机发命令时电机不在发送，反之亦然（请求-响应），不会互相压制。

---

## 3. 控制报文

### 3.1 定标函数（与手册一致）

```c
float uint_to_float(uint32_t x, float x_min, float x_max, uint32_t bits) {
    float span = x_max - x_min;
    return (float)x * span / (float)((1u << bits) - 1u) + x_min;
}
```

线缆定标常量（编译期，`app_can_protocol.h`）：

| 常量 | 值 | 用于 |
| :--- | :--- | :--- |
| `P_MAX` | 12.5 | MIT p_des、反馈 POS |
| `V_MAX` | 30.0 | MIT v_des、反馈 VEL |
| `T_MAX` | 10.0 | MIT t_ff（保持 DM 兼容） |
| `T_FB_MAX` | 200.0 | 反馈 T（本电机全量程，覆盖 30 A→175.5 Nm） |
| `KP_MAX` | 500.0 | MIT kp 解码 |
| `KD_MAX` | 5.0 | MIT kd 解码 |

> 这些是**线缆契约**，不是本机能力上限。本机实际限幅（`control.mit.kp_max/kd_max`、`motion_iq_max_a`、`motion_speed_limit_rad_s`）仍在 motion/FOC 层生效。
>
> 备选：把 `P_MAX/V_MAX` 调整为机械匹配值可提高分辨率，但与 DM 主机不兼容。
> 反馈 T 已使用独立量程 `T_FB_MAX=200`（见 §10 第 1 项）；MIT `t_ff` 仍保持 ±10 Nm，
> 因此 DM 主机的控制输入不受影响。

### 3.2 MIT 模式（ID = `0x11X`，8 字节）

| 字节 | 位 | 字段 |
| :--- | :--- | :--- |
| D0 | 7:0 | `p_des[15:8]` |
| D1 | 7:0 | `p_des[7:0]` |
| D2 | 7:0 | `v_des[11:4]` |
| D3 | 7:4 | `v_des[3:0]` |
| D3 | 3:0 | `kp[11:8]` |
| D4 | 7:0 | `kp[7:0]` |
| D5 | 7:0 | `kd[11:4]` |
| D6 | 7:4 | `kd[3:0]` |
| D6 | 3:0 | `t_ff[11:8]` |
| D7 | 7:0 | `t_ff[7:0]` |

解码：

```text
p_des = uint_to_float(D0<<8 | D1,        -P_MAX, +P_MAX, 16)
v_des = uint_to_float(D2<<4 | D3>>4,     -V_MAX, +V_MAX, 12)
kp    = uint_to_float((D3&0x0F)<<8 | D4,  0.0,   KP_MAX, 12)
kd    = uint_to_float(D5<<4 | D6>>4,      0.0,   KD_MAX, 12)
t_ff  = uint_to_float((D6&0x0F)<<8 | D7, -T_MAX, +T_MAX, 12)
```

单位：`p_des` 输出轴 rad，`v_des` 输出轴 rad/s，`kp` Nm/rad，`kd` Nm/(rad/s)，`t_ff` Nm，均为用户坐标（已含 `software.motion.output_direction`）。

**映射到运动层**

```c
/* 单圈兼容：把 p_des 映射到「距当前连续角最近」的目标 */
target_cont = shaft_continuous_user
            + wrap_pm_pi(p_des - wrap_pm_pi(shaft_single_turn_user));
app_motion_mit(target_cont, v_des, kp, kd, t_ff);
```

- 前提：`shaft_continuous_rad` 可信（`continuous_valid`），否则丢帧并计数。
- `kp`/`kd` 在进入 motion 前钳到 `control.mit.kp_max/kd_max`（CAN 入口静默钳位；终端
  `motion mit` 拒绝超限）。**这是输入增益钳位，不是输出转矩上限**；输出转矩仍由
  `pos_err_max_deg`（90°）与电流限幅（24.3 A 连续 / 30 A boost）约束。
- MIT 仍需 `control.mit.enable = true`；未开启时丢帧并计数。
- 位置误差受 `control.mit.pos_err_max_deg`（默认 90°）限制；超出即被 motion 拒绝。需要更大行程时改用位置速度模式。

### 3.3 位置速度模式（ID = `0x12X`，8 字节）

| 字节 | 字段 |
| :--- | :--- |
| D0..D3 | `p_des`，float32 小端（输出轴 rad） |
| D4..D7 | `v_des`，float32 小端（最大速度，输出轴 rad/s，取绝对值） |

处理：

1. 非有限值 → 丢帧。
2. `v_des` 取 `|v_des|`，钳到 `[0.01, V_MAX]`；0 或负值按 0.01 处理（避免「永不动」）。
3. `p_des` 按单圈处理：`p_user = wrap_2pi(p_des)`，转成 `[0, 2π)`。
4. 调用（需新增接口，见 §7）：

```c
app_motion_position_limited(p_user, v_lim, 0.0f, 0.0f);
```

### 3.4 速度模式（ID = `0x13X`，4 字节起）

| 字节 | 字段 |
| :--- | :--- |
| D0..D3 | `v_des`，float32 小端（输出轴 rad/s） |

处理：非有限值丢帧；其余直接 `app_motion_speed(v_des)`。

DLC 接受 4..8，只取前 4 字节（手册示例为 4）。

### 3.5 力位混控模式（ID = `0x14X`，8 字节）

| 字节 | 字段 |
| :--- | :--- |
| D0..D3 | `p_des`，float32 小端（输出轴 rad） |
| D4..D5 | `v_des`，uint16 小端，×100 → 0..100 rad/s（>10000 钳到 10000） |
| D6..D7 | `i_des`，uint16 小端，×10000 → 0..1.0 标幺（>10000 钳到 10000） |

处理：

1. `p_des` 单圈处理同 §3.3。
2. 速度限幅 `v_lim = v_des_raw / 100.0f`。
3. 电流限幅 `iq_lim = (i_des_raw / 10000.0f) × I_MAX_PHASE_A`。
   - `I_MAX_PHASE_A` 取 `motor.i_peak_10s_a`（当前 24.3 A，峰值口径），再与运行期 `motion_iq_max_a` / boost 动态限幅取小。
4. 调用：

```c
app_motion_position_limited(p_user, v_lim, 0.0f, iq_lim);
```

> 手册的 `i_des` 是「实际电流 / 最大相电流」的标幺。本机以 `i_peak_10s_a` 作为分母，保证与上电打印/铭牌一致。

---

## 4. 使能 / 失能 / 清故障（扩展帧）

手册未定义，本设计沿用「ID 偏移 + 8 字节载荷」的同一风格，并加 **magic 字节**，防止总线上一条全 0 帧误使能。

| ID | D0 | D1..D7 | 行为 |
| :--- | :--- | :--- | :--- |
| `0x15X` | `0xFC` | 0 | `app_foc_enable()` |
| `0x16X` | `0xFD` | 0 | `app_motion_stop()`（受控软停） |
| `0x17X` | `0xFE` | 0 | 仅 FOC OFF 时 `app_fault_clear()` |

约束：

- 使能失败（未标定、故障锁存、FOC 非 OFF 等）不置故障，只计数；反馈 ERR 仍为 0/故障码，主机可据此重试。
- 失能走 `app_motion_stop()`，复用现有软停机（电流归零、轴端速度收敛、再关栅极供电），**不做硬件急停**。
- 清故障只清锁存，不自动使能；主机需再发使能帧。`app_fault_clear()` 本身会复检故障条件，条件仍在时拒绝。
- **TRIP 后的恢复顺序**：`0x16X` 失能（FAULT→OFF）→ `0x17X` 清锁存 → `0x15X` 使能。FAULT 态下直接清锁存不会让 FOC 回到 OFF。
- 未按 magic 的帧一律丢弃并计数（不回复）。

**为什么不用 `0xFC/0xFD` 精确 ID**：单总线多节点时无法寻址，且 0xFC/0xFD 不在本方案 `0x1XX` 区间。`0x15X/0x16X/0x17X` 与四类控制帧共用「低 4 位节点号」规则，天然按节点寻址。

---

## 5. 反馈报文（ID = `0x18X`，8 字节）

| 字节 | 内容 |
| :--- | :--- |
| D0 | `(ERR << 4) | (node_id & 0x0F)` |
| D1 | `POS[15:8]` |
| D2 | `POS[7:0]` |
| D3 | `VEL[11:4]` |
| D4 | `(VEL[3:0] << 4) | (T[11:8])` |
| D5 | `T[7:0]` |
| D6 | `T_MOS`（℃） |
| D7 | `T_Rotor`（℃） |

编码：

```text
POS = float_to_uint(pos_user,   -P_MAX, +P_MAX, 16)
VEL = float_to_uint(vel_user,   -V_MAX, +V_MAX, 12)
T   = float_to_uint(torque_user,-T_FB_MAX, +T_FB_MAX, 12)
```

```c
// 与手册 float_to_uint 一致：截断，不四舍五入
uint32_t float_to_uint(float x, float min, float max, uint32_t bits) {
    float span = max - min;
    return (uint32_t)((x - min) * (float)((1u << bits) - 1u) / span);
}
```

### 5.1 物理量来源

| 量 | 来源 | 说明 |
| :--- | :--- | :--- |
| 位置 | `app_motion_snapshot.shaft_rad × output_direction`，再折到 `[-π, π)` | 以 `motion zero` 为零点；未标定/`feedback_valid=0` 时置 0 且 ERR=F；**速度/力矩模式需先打开角度回报（§7.2），否则 `shaft_rad` 会冻结**。游标失配（`angle_valid=0`）只降低位置精度，不判故障 |
| 速度 | `snapshot.speed_rad_s × output_direction` | 输出轴 rad/s |
| 转矩 | `kt_nm_per_a × i_q_measured × reducer_ratio × output_direction` | 电流估算值，非转矩传感器；限幅 ±T_FB_MAX |
| `T_MOS` | NTC0 换算温度 | 见 §5.3 |
| `T_Rotor` | NTC1 换算温度 | 本板 NTC1 当前开路 → `0xFF` |

> 转矩不使用 `snapshot.torque_nm`（那是 motion 请求值，限幅/保持时与实际不符），而用 FOC 实测 q 轴电流换算。

### 5.2 ERR 映射与优先级

| ERR | 手册含义 | 本工程来源 |
| :--- | :--- | :--- |
| 0 | 失能 / 无故障 | FOC OFF 且无锁存 |
| 1 | 使能正常 | FOC READY / RUN |
| 8 | 超压 | `APP_FAULT_VBUS_OV` |
| 9 | 欠压 | `APP_FAULT_VBUS_UV` |
| A | 过流 | `OC_FAST_U/V/W`、`OC_SLOW_U/V/W`、`OC_TRIP` |
| B | MOS 过温 | NTC0 超阈值（阈值待定，见 §10） |
| C | 线圈过温 | NTC1 超阈值（硬件当前开路） |
| D | 通讯丢失 | `APP_FAULT_COMM_TIMEOUT` |
| E | 过载 | 预留（本机过载表现为 `OC_SLOW` → A） |
| F | 其他 | `ADC_TIMEOUT`、`ENC_READ`、反馈无效、未知 |

多故障并发时按固定优先级取一个：**A > 8 > 9 > B > C > D > F**。

### 5.3 温度换算与哨兵

NTC 阻值 → 摄氏温度（B 参数法）：

```text
T[℃] = 1 / (1/298.15 + ln(R/R25)/B) - 273.15
```

- `R25` / `B` 取 `hardware.ntc.r25_ohm` / `b_value_k`（当前为占位值，型号确定后修正）。
- 阻值超出 `(100 Ω, 1 MΩ)` 或非有限 → 判无效。
- 无效温度上报 `0xFF`（255）。主机必须把 `0xFF` 当「未知」，不能当温度值。

### 5.4 回复策略

- 每收到一条**通过校验**的帧（控制 / 使能 / 失能 / 清故障）回一帧。
- 长度错误、非本机、magic 错误：不回帧，只计数。
- 每 1kHz 拍最多回 `8` 帧（与接收预算一致），超出丢弃并计数。
- 反馈在 1kHz 任务内组装，从收帧到回帧 ≤ 1 ms + CAN 发送时间。
- **命令被 motion 拒绝时 ERR 不变**（拒绝不是故障），只累加计数；拒绝原因走 USB 诊断。主机的运行期判据是「反馈是否跟随」，不是 ERR。

> 分辨率：16 位 POS 映射到 ±12.5 rad 时为 0.38 mrad/LSB；本机实际只使用 `[-π, π)` 段（约 26% 码值），与 DM 相同。需要更高分辨率时再讨论 §10 第 1 项。

---

## 6. 状态机与时序

```text
上电 → 失能（ERR=0）
   ├─ 0x15X（magic 0xFC）→ app_foc_enable() → READY（ERR=1）
   ├─ 控制帧 → 运动（需已使能；未使能则丢帧并计数）
   ├─ 0x16X（magic 0xFD）→ app_motion_stop() → OFF（ERR=0）
   ├─ 故障 → STOP 软停 / TRIP 关桥；ERR=对应故障码（STOP 不置 FAULT）
   └─ 0x17X（magic 0xFE，仅 OFF）→ 清锁存
```

### 6.1 模式切换

DM 手册通过「模式寄存器」切换；本设计由帧 ID 隐式决定模式。约束：

- 运动进行中收到**不同模式**的帧：调用一次 `app_motion_release_output()`（零力矩、撤销 motion 输出，但**保持 FOC 使能**），丢弃该帧并计数；下一拍起接受新模式命令。
- 原因：`app_motion` 有 `MODE_LOCKED`（活动期间禁止切模式），强行切换会变成静默拒绝。显式释放输出比静默失败可观测，且比 `app_motion_stop()` 少一次「停桥—再使能」。
- 同一模式内连续帧属于正常跟踪，不触发释放。
- 若主机希望彻底停机，仍用失能帧（`0x16X`），不要依赖模式切换。

### 6.2 命令前置条件（会被 motion/FOC 拒绝的情形）

CAN 服务只做协议与坐标转换，以下条件仍由现有模块判定；不满足时丢帧并计数（不回错误码）：

- 已使能（FOC READY/RUN）且未进入停机流程；
- 已完成 `cal encoder` / `motion cal`（`calibrated`），反馈新鲜；
- 位置/速度模式已配置环路增益（`GAIN_NOT_SET`）；
- MIT 需 `control.mit.enable=true`；
- 位置行程受 `max_step_deg` / `max_travel_deg` / 软限位约束（单圈命令天然满足）。

### 6.3 与现有看门狗的关系

- 任何通过校验的本机帧都喂通信看门狗（保留 2026-10-28 的双源设计；USB 终端输入同样喂）。
- 超时 → `APP_FAULT_COMM_TIMEOUT`，动作由 `fault` policy 决定。
- **要让「CAN 静默自动退出使能」与 DM 行为一致**，需要 `fault.actions_enable=true` 且 `COMM_TIMEOUT` 的 policy 为 STOP/TRIP。当前出厂 `actions_enable=false`（只告警），见 §10。

---

## 7. 需要的跨层改动

### 7.1 `App/Comm/can/`

| 文件 | 改动 |
| :--- | :--- |
| `app_can_protocol.[ch]` | **重写**：DM 编解码纯函数（控制帧、反馈帧、定标、ID 解析、温度/错误映射）。不再含 CANSimple 命令枚举 |
| `app_can_service.[ch]` | **重写**编排：1kHz 解析 → 执行 → 回帧；看门狗/故障/模式切换 |
| 删除 | CANSimple 的 `SET_AXIS_STATE / SET_INPUT_* / HEARTBEAT / GET_*` 等命令与用例 |

`app_can` 平台层（`App/Platform/Src/app_can.c`）**不变**：收发、环形队列、过滤、统计继续复用。

### 7.2 `App/Control`

1. 新增 `app_motion_position_limited(pos, vel_limit, torque_ff, iq_limit)`：
   - 位置命令前设置本次轨迹的 `v_max`（`algo_ramp` 需要新增 `set_limits()` 或允许在 `plan()` 前改 `_v_max`）；生效值 = `min(vel_limit, YAML trajectory_vel_limit)`，YAML 值仍作全局安全上限。
   - `iq_limit` 作为本次命令的电流上限；生效值 = `min(iq_limit, motion_iq_max_a, 动态 boost 限幅)`；0 = 不额外限制。
   - 不改变现有 `app_motion_position()` / `app_motion_position_multi()` 的语义。
2. **新增 `app_motion_set_angle_report_required(bool)`**（与 `s_output_tracking` / `s_cogging_output_required` 并列，OR 进 `need_pair`）：
   - 原因：速度/力矩模式下 `app_motion` 默认不读输出编码器 pair，`shaft_rad` 保持上一值（冻结）；DM 反馈要求任何模式都能报位置。
   - CAN 服务在初始化时置 true；代价与位置模式相同（每 15kHz 拍读一次输出 pair + 游标解算）。
   - 不能复用 `app_motion_set_output_tracking()`：齿槽标定结束时会把该标志清零，会顺带关掉 CAN 的位置回报。
3. `app_motion_mit()` 已满足五元组需求，不改。
4. `App/Platform/Src/app_analog_signal.c` 新增 NTC 阻值 → ℃（纯函数，输入 R，输出 ℃ 或 NAN）。
5. 新增节点号读取与 ADC 槽释放：
   - `app_can_id_read()`：等待 ADC1 至少一帧 → `app_adc_slow_process()` → `app_adc_get_raw(ADC_CH_V_CANID)` → 阈值解码为电压档位 → level→ID 置换（ID = 拨码二进制，0..7）；失败回退 YAML；结果写 RAM。
   - `app_adc_canid_release()`：停止 ADC1 SEQ → 重写 4 槽 → 重启；同时把 `s_slow_channels` 降到 3 项。
   - 调用点放在 `app_init()` 中 `app_adc_init()` 之后（新步骤 11b）；`app_can_service_init()` 随之从步骤 5 后移，`app_debug_can_init()` 仍留步骤 5。
   - 读取流程：等 2 ms 让 ADC1 序列稳定 → 9 次 `app_adc_slow_process()`（间隔 1 ms）→ `app_adc_get_raw(ADC_CH_V_CANID)` → 中值 + ±256 LSB 内点过滤 → level→ID 解码 → 释放 CANID 槽 → `app_can_service_init(node_id)`。


### 7.3 配置与存储

`app_software_params_t`（SPRM）当前 232 字节，记录上限 236（`APP_PARAM_DATA_MAX=240`，头 4 字节）。**SPRM 是前缀迁移**（`APP_SOFTWARE_PARAMS_V1_SIZE` 为旧记录长度），字段一旦删除/移动，旧 flash 记录的后续字段会整体错位。因此本次**不删字段、不改布局**：

| 字段 | 动作 |
| :--- | :--- |
| `baudrate` | 保留 |
| `node_id_default` | **原地改名** `can_id`（u8，YAML 兜底值；偏移不变） |
| `rx_control_id` | 保留为 `reserved_rx_id`（占位，不参与逻辑） |
| `tx_report_id` | 保留为 `reserved_tx_id`（占位，不再参与逻辑） |
| 新增字段 | 不新增 |

`control.mit.can_format` 同样**保留为 reserved**（1 字节），原因相同：它在 `storage` 之前，删除会让 V1 前缀迁移失效。等将来真正需要扩容时，再一次性提升记录代次并写迁移。

阈值表放在 `App/Platform/Src/app_can_id.c` 的 `static const uint16_t s_canid_thresholds[7]`（16 位 ADC 码，见 §2.3.1 最终表）；生成器不支持 YAML 数组，且该表是板级事实、不需要运行期修改。`hardware.yaml` 的 `canid_dip.levels` 改为 8 作文档。阈值未配置（全 0）时判无效并回退 YAML。

线缆定标常量（P_MAX/V_MAX/T_MAX/T_FB_MAX/KP_MAX/KD_MAX）为编译期常量，不入 flash；改它们需要重烧两侧。`T_MAX` 只用于 MIT t_ff，`T_FB_MAX` 只用于反馈 T；两者分离以保持 DM 控制输入兼容。若后续需要上位机可调，再新增独立 `APP_PARAM_KEY_CAN` 记录。

---

## 8. 测试计划

### 8.1 主机测试（`scripts/tests/can/`）

| 用例 | 断言 |
| :--- | :--- |
| ID 结构解析 | `0x110..0x19F`：type 1..9、node 0..15；`0x1AX`/`0x0XX`/`0x2XX` 一律不匹配；扩展帧拒绝 |
| 节点号解码 | 8 档阈值 + 电压档位→ID 置换（ID = 拨码二进制）：边界值、越界回退 YAML |
| MIT 位打包 | 手册公式向量；边界 0 / 满量程 / 中点；与独立实现的逐位对照 |
| MIT 往返 | 编码 → 解码误差 ≤ 1 LSB |
| 反馈打包 | D0 的高/低半字节、D3/D4 的 12 位拆分、负值补码 |
| 定标饱和 | p=±P_MAX、v=±V_MAX、t_ff=±T_MAX（MIT）、T=±T_FB_MAX（反馈）、越界钳位 |
| 位置折返 | `-π/π` 边界、`wrap_2pi`、MIT 最近圈映射 |
| ERR 映射 | 单码、多码优先级、无故障 0/1 |
| 温度 | 正常 R、开路、短路、`0xFF` 哨兵 |
| 异常帧 | DLC 错误、magic 错误、NaN/Inf、非本机 ID |

### 8.2 台架测试

1. 用 USB-CAN / 第二块 MCU 以 1 kHz 发四类帧，逐项核对反馈。
2. 使能 → 速度 → 位置 → 力位混控 → 失能，确认 ERR 与状态一致。
3. 断开 CAN，确认 `COMM_TIMEOUT` 的动作（需 `actions_enable=true`）。
4. 故障注入（`fault inject`）验证 ERR 码。
5. 1 kHz 持续收发 10 分钟，检查丢帧/溢出/CPU 占用。
6. ADC1 槽数 5→4 后：重跑 §9.5.2 vbus 配对 A/B 与 `cal rl`；`adc diag` 确认帧率仍 ≈ pwm_freq、`vbus_fast` 无 stale 增长。
7. DIP 逐档上电（S1..S3 共 8 档；S4 开/关各测一次），核对 §2.3.1 的最终表与反馈 ID（`0x18X`）一致。

---

## 9. 迁移与删除清单

| 项 | 处理 |
| :--- | :--- |
| `app_can_protocol.c/h` 的 CANSimple 编解码 | 删除，替换为 DM 版本 |
| `app_can_service.c` 的 CANSimple 命令分发 | 删除，替换为 DM 版本 |
| `scripts/tests/can/` 的 CANSimple 用例 | 重写 |
| `config/software.yaml` 的 `rx_control_id` | 保留为 reserved（不参与逻辑） |
| `config/software.yaml` 的 `tx_report_id` | 保留为 reserved（不参与逻辑） |
| `config/software.yaml` 的 `control.mit.can_format` | 保留为 reserved（删除会破坏 SPRM 前缀迁移） |
| `app_adc.c` 的 ADC1 5 槽冻结配置 | 增加运行期 4 槽配置与 `app_adc_canid_release()`；重跑 §9.5.2 A/B 与 `cal rl` |
| `app_analog_signal` 的 CANID 通道 | 读取一次后标记失效，不再作为实时物理量 |
| `AGENTS.md` CAN 章节 | 实施后更新（含 ID 方案与 ADC 槽变化） |

---

## 10. 待确认项（评审重点）

1. **定标范围**：P/V/KP/KD 沿用 DM 默认 `12.5/30/500/5`；MIT `t_ff` 保持 ±10 Nm（DM 兼容），
   **反馈 T 改为本电机全量程 ±200 Nm**（DM 主机读 T 需改常量）。若要求反馈也完全兼容 DM，
   可改用 `0x190` 扩展帧携带全量程转矩。
2. ~~节点号阈值表~~：已实测确认（§2.3.1，8 档，2026-11-02）。固件按该表实现；返修 R75/R76 后需重测。
3. **模式切换**：运行中切模式采用「释放输出（零力矩、保持使能）→ 下一拍接受新模式」，是否可接受？（DM 本身可在运行中切换，本机 motion 有 `MODE_LOCKED` 限制。）
4. **MIT 位置语义**：按单圈最短路径映射（推荐，贴近 DM），还是要绝对连续坐标？
5. **`i_des` 分母**：用 `motor.i_peak_10s_a`（24.3 A）还是 `control.limits.i_q_max_a`（30 A）？
6. **转矩反馈口径**：用实测 iq × Kt × 减速比（推荐），还是 motion 请求值？
7. **温度映射**：NTC0 = MOS、NTC1 = 电机线圈，是否符合板级实际？NTC1 当前开路，是否先上报 `0xFF`？
8. **`COMM_TIMEOUT` 动作**：是否为本机出厂打开 `fault.actions_enable=true` 并把 `COMM_TIMEOUT` 设为 STOP/TRIP（即 DM 的「静默退出使能」）？

---

## 11. 风险

| 风险 | 影响 | 缓解 |
| :--- | :--- | :--- |
| 位打包与 DM 主机不一致 | 电机乱动 | 主机测试用独立位实现交叉验证；台架先低速小电流 |
| 单圈位置映射错圈 | 位置跳到相邻圈 | MIT 用最近圈；位置模式强制 `wrap_2pi`；`max_step` 保护仍在 |
| 模式切换被静默拒绝 | 主机以为已切换 | 不同模式帧触发一次 `app_motion_release_output()` + 计数；下一拍起接受新模式 |
| 速度/力矩模式下位置反馈冻结 | 反馈位置恒定 | CAN 初始化即打开 `app_motion_set_angle_report_required(true)`；台架用位置/速度两种模式对照反馈 |
| `COMM_TIMEOUT` 默认只告警 | 拔线不停机 | §10 第 8 项决策；台架验收明确检查 |
| NTC1 开路 | `T_Rotor` 恒为哨兵 | 文档标注；硬件修复前主机不依赖该值 |
| 反馈 1kHz 占 CPU | 1kHz 任务变长 | 每拍最多 8 帧；组帧为纯字节操作；实测 `profiler dump` 复核 |
| ADC1 5→4 槽改变既有台架结论 | vbus/cal rl 落点漂移 | 按 §8.2 第 6 条重跑 A/B 与 cal rl；首槽 VBUS 不变，风险集中在整帧时长 |
| DIP 阈值未标定 | 节点号误判/冲突 | 阈值未配置时回退 YAML 并在启动日志告警；整机装配前逐档实测 |

---

## 12. 实施顺序（建议）

1. **协议纯函数**：`app_can_protocol.[ch]` + 主机测试（不碰硬件，可先合并）。
2. **motion 支撑改动**：`algo_ramp` 限幅设置、`app_motion_position_limited()`、`app_motion_set_angle_report_required()`、NTC 温度换算 + 主机测试。
3. **节点号与 ADC 槽**：DIP 8 档阈值实测 → `app_can_id_read()` → `app_adc_canid_release()` → 调整 `app_init()` 顺序；重跑 §9.5.2 A/B 与 `cal rl`。
4. **服务层重写**：`app_can_service.[ch]`，接入 motion/FOC/故障；保留现有 RX 队列与看门狗。
5. **配置迁移**：`node_id_default` → `can_id` 原地改名（YAML 兜底）、移除 CANSimple 用例、更新 AGENTS。
6. **台架验收**：§8.2，先低速小电流，再 1kHz 长跑。

第 1、2 步可独立合并；第 3 步起需要台架（ADC 重配与阈值标定）。
