# G6618 电机 USB 终端命令手册 v0.1.1

| 项目     | 内容                                                                                         |
| :------- | :------------------------------------------------------------------------------------------- |
| 适用对象 | HPM53M1 + G6618 电机（50:1 减速器）                                                          |
| 适用固件 | `HPM53M1_G6618Motor`（2026-11-03 起）                                                      |
| 物理通道 | USB0 CDC 虚拟串口（`PAxx` USB 口），CherrySH 命令层                                        |
| 提示符   | `HPM53M1@G6618:/$`                                                                         |
| 角色     | 调试、标定、参数整定；**CAN 运行时控制见 [can-protocol_v0.1.1.md](can-protocol_v0.1.1.md)** |

![1793715482879](image/usb-terminal_v0.1.1/1793715482879.png)

---



## 1. 通用约定

### 1.1 连接

- 任意波特率（USB CDC 虚拟串口不依赖波特率，建议 115200）。
- 主机必须拉高 **DTR**；`DTR=0` 时终端输出会被直接丢弃（`usb` 命令可查）。
- 上电后等待启动自检完成（最后一行进入 `HPM53M1@G6618:/$`）再输入命令。
- 终端与 CAN 可同时工作；CAN 控制帧和 USB 终端输入都会喂通信看门狗。

### 1.2 单位（重要）

终端命令使用**输出轴**的工程单位，与 CAN 协议的 rad/rad/s 不同：

| 量     | 终端                                                                          | CAN                          |
| :----- | :---------------------------------------------------------------------------- | :--------------------------- |
| 位置   | `deg`（`motion pos` 为 0~360 单圈；`pos_multi/pos_rel/mit` 为连续 deg） | `rad`                      |
| 速度   | `rpm`（输出轴）                                                             | `rad/s`                    |
| 转矩   | `Nm`（输出轴）                                                              | `Nm`（反馈 T 量程 ±200）  |
| MIT Kp | `Nm/rad`（轴端）                                                            | `Nm/rad`                   |
| MIT Kd | `Nm/(rad/s)`（轴端）                                                        | `Nm/(rad/s)`               |
| 电流   | `A`（FOC q/d 轴物理电流）                                                   | `A`（i_des 标幺 ×24.3 A） |

### 1.3 长操作与阻塞行为

- `cal current` / `cal encoder` / `cal rl` / `cal inertia` / `cal cogging` / `motion friction cal` / `foc trace` 以 **job** 形式运行。
- **job 运行期间终端输入被丢弃**，不会因按键中止；等待完成或超时后再操作。
  （部分提示仍写 "any key aborts"，以实际行为为准。）
- `foc off` / `motion off` 会阻塞等待受控停机，最长约 3 s。
- flash 写入（`param save`、`enc zero`、各 `cal ... save`、`motion save`）会短暂关闭中断，USB 可能停顿，**不要在此期间拔线/断电**。

---

## 2. 命令索引

| 命令                                            | 作用                                       | 关键前置                  |
| :---------------------------------------------- | :----------------------------------------- | :------------------------ |
| `ver`                                         | 固件版本、构建时间、复位源                 | —                        |
| `status`                                      | 故障/模拟量/编码器快照                     | —                        |
| `usb`                                         | USB 链路与 Terminal 统计                   | —                        |
| `clear`                                       | 清屏                                       | —                        |
| `reboot confirm`                              | 软件复位                                   | 电机停止                  |
| `driver power [on\|off]`                       | +12V 栅极供电手动开关                      | FOC OFF                   |
| `can [id]`                                    | CAN 节点/总线/协议统计；`id` 看 DIP 快照 | —                        |
| `adc dump\|diag\|delay <ns>`                    | ADC 通道/速率/触发延时                     | —                        |
| `pwm dump`                                    | HRPWM 比较值与频率                         | —                        |
| `enc info\|zero <rotor\|output>\|clear`          | 编码器读数/置零/清零点                     | 电机停止                  |
| `fault show\|clear\|protect\|policy\|inject`      | 故障锁存与动作策略                         | —                        |
| `profiler dump`                               | 逐 ISR 频率/耗时/outliers                  | —                        |
| `monitor [on\|off]`                            | 常驻状态区（2 Hz）                         | —                        |
| `foc status\|on\|off\|vtest\|dutycap\|bench\|trace` | FOC 使能/电压测试/调制上限/台架/捕获       | 见 §5                    |
| `motor iq [A]\|motor id [A]`                   | 直接 q/d 轴电流                            | FOC READY/RUN、DIRECT 源  |
| `motion ...`                                  | 运动模式、标定、MIT                        | 见 §7                    |
| `cal ...`                                     | 编码器/RL/惯量/齿槽/电流零点标定           | 见 §8                    |
| `cogging ...`                                 | 齿槽/传动角表开关与增益                    | 见 §9                    |
| `param list\|get\|set\|reset\|save`               | 参数访问与持久化                           | `save/reset` 需 FOC OFF |

---

## 3. 系统与状态

### 3.1 `ver`

```text
ver
```

打印固件版本、构建日期/时间、`reset status`。

### 3.2 `status`

```text
status
```

一次性快照：

```text
fault   state=... codes=0x... (名称)
        latched=0x... (名称) first=0x...
analog  i_u=...A i_v=...A i_w=...A v_bus=...V ntc0=...ohm ntc1=...ohm
encoder rotor=... deg output=... deg param=valid|default loop_late=...us
```

### 3.3 `usb`

```text
usb
```

DTR、Terminal 就绪、TX 丢弃字节、CDC `tx_timeouts/tx_drops/rx_drops/bus_resets/xfer_errors`。
`tx_timeouts` 应保持 0（端点卡死强制恢复计数）。

### 3.4 `clear`

```text
clear
```

清屏并重绘提示符。

### 3.5 `reboot confirm`

```text
reboot confirm
```

二次确认 + 电机停止联锁后软件复位。**必须带 `confirm`**。

### 3.6 `driver power [on|off]`

```text
driver power            # 查询
driver power on         # 开 +12V 栅极供电（PWM 仍关闭）
driver power off        # 关 +12V
```

要求 FOC OFF 且无 stop pending。用于单独验证栅极供电，不会使能 PWM。

---

## 4. 诊断

### 4.1 `can [id]`

```text
can        # node / wd / bus(tec,rec,ep) / dm rx,drop,bad,foreign,switch,rej,txf
can id     # 上电 DIP 快照：dip/raw/spread + ADC 槽状态
```

### 4.2 `adc dump|diag|delay <ns>`

```text
adc dump        # 原始码/mV/物理量；NTC 行附 temp=
adc diag        # ADC0 速率/ISR 占用、ADC1 SEQ、vbus_fast、寄存器
adc delay <ns>  # 运行期改 PWM1 CMP10 触发延时（默认 500 ns）
```

`adc diag` 的 `rate/isr_cpu` 带自校验：**连续执行两次、间隔 <8.9 s**（cycle 计数 32 位回绕）。

### 4.3 `pwm dump`

```text
pwm dump
```

HRPWM 比较值与频率。

### 4.4 `enc info|zero <rotor|output>|clear`

```text
enc info                 # 两路角度/计数/零点
enc zero rotor           # 转子编码器置零（写 flash）
enc zero output          # 输出编码器置零（写 flash）
enc clear                # 清除两路零点（写 flash）
```

`zero/clear` 要求电机停止；会短暂停顿（扇区擦除）。

### 4.5 `fault show|clear|protect|policy|inject`

```text
fault show                          # 故障快照与策略
fault clear                         # 条件满足时清锁存
fault protect [on|off]              # 动作总开关（运行期 RAM）
fault policy [show]                 # design/live 动作表
fault policy <code> <warn|derate|stop|trip>
fault inject <code> on|off          # 台架注入（不经物理阈值）
```

可用 `<code>`（大小写不敏感）：

```text
OC_FAST_U / OC_FAST_V / OC_FAST_W
OC_SLOW_U / OC_SLOW_V / OC_SLOW_W
VBUS_OV / VBUS_UV
ADC_TMO / ENC_ERR / OC_TRIP
```

> 已知缺口：`COMM_TIMEOUT` 目前只在状态/文本位图里出现，**不在** `fault policy/inject`
> 的名称表中；要注入通信超时需先补名称表。

### 4.6 `profiler dump`

```text
profiler dump
```

逐 ISR 的频率/平均/峰值/outliers，用于快环路 CPU 盘点。

### 4.7 `monitor [on|off]`

```text
monitor          # 切换
monitor on       # 打开常驻状态区（2 Hz；Ctrl-C 或 monitor off 关闭）
monitor off
```

状态区绘制在提示符上方，不阻塞命令输入。

---

## 5. FOC

### 5.1 状态与使能

```text
foc [status]     # 默认；state/theta/omega/i_d/i_q/vbus/限幅
foc on           # 使能；要求无 STOP/TRIP 锁存、电机停止
foc off          # 受控软停（最长 3 s），完成后关 +12V
```

`foc off` 走正常停机顺序：先关 +12V、等待 20 ms、再停 PWM；不要在高速时直接断总线电源。

### 5.2 电压测试

```text
foc vtest <volts> [<deg>]   # 固定电角度施加电压 1.5 s
foc vtest off               # 立即停止
```

要求 FOC READY/RUN；`<deg>` 为电角度。用于核对相序、电流符号与采样映射，输出力矩很小。

### 5.3 调制上限

```text
foc dutycap            # 查询
foc dutycap 0.95       # 设置 [0.70, 0.98]，仅 OFF，RAM；重启回 0.95
```

改动后必须复核低侧采样窗口。

### 5.4 台架模式

```text
foc bench 1    # 关闭自动停机类保护：i_trip=0、foc_speed_max=0、actions=warn-only
foc bench 0    # 恢复 YAML 默认
```

仅用于限流电源台架；不改变控制律。恢复后 `fault protect` 的状态按默认值。

### 5.5 捕获

```text
foc trace
```

要求 FOC READY/RUN；短时捕获 30 kHz 波形（约 4 ms），完成后经 job 打印。

---

## 6. 直接电流

```text
motor iq            # 查询 q 轴给定/平均/瞬时
motor iq <A>        # 设置 q 轴电流
motor id            # 查询 d 轴
motor id <A>        # 设置 d 轴电流（同时把 iq 清零）
```

要求 FOC READY/RUN，且电流源为 **DIRECT**（`foc on` 后默认；若之前跑过 motion，先
`motion off` / `foc off`，必要时 `motion direct`）。`i_q` 受 `control.limits.i_q_max_a` 限制。

---

## 7. 运动

```text
motion show
motion cal                      # 静止相位标定（FOC OFF、motion idle）
motion apply                    # 应用相位（FOC OFF、已完成 cal；RAM；旧零点失效）
motion zero [save]              # 当前位置置零（FOC OFF、已标定、反馈新鲜；save 同时写 flash）
motion save                     # 相位+零点写 flash（FOC OFF、已标定）
motion off                      # 撤销运动输出并受控停机（等待 FOC OFF + 关 +12V，最长 3 s）
motion direct                   # 切 DIRECT 电流源（仅 FOC OFF）
```

> **运动命令通用前置**：已 `cal encoder` + `motion cal/apply`、反馈新鲜、FOC READY/RUN；
> 位置模式还要求 `pos_gain` 与速度环增益已配置；模式切换需先 `motion off`。

### 7.1 增益

```text
motion gains <kp> <ki> [coulomb] [viscous]    # 速度环；idle
motion pos_gains <pos_gain> [bw]              # 位置环；idle
```

- `kp` [Nm/(rad/s)]、`ki` [Nm/rad]、`coulomb` [Nm]、`viscous` [Nm/(rad/s)]；
- `pos_gain` [1/s]、`bw` [rad/s]（0 = 直通）；
- 运动中不允许改，先 `motion off`。

### 7.2 命令

```text
motion torque <Nm>
motion speed <rpm>
motion pos <deg[0~360]> [vel_ff_rad_s] [torque_ff_nm]
motion pos_multi <deg> [vel_ff_rad_s] [torque_ff_nm]
motion pos_rel <deg> [vel_ff_rad_s] [torque_ff_nm]
```

- `pos`：输出轴单圈绝对位置，走最短路径；360° 归 0。
- `pos_multi`：连续坐标绝对位置（可为负/多圈），要求梯形轨迹已配置。
- `pos_rel`：相对当前连续角的增量。
- 位置命令的速度/力矩前馈单位分别为 `rad/s`、`Nm`。

### 7.3 MIT

```text
motion mit <p_deg> <v_rpm> <kp> <kd> <t_ff>
motion mit_here <v_rpm> <kp> <kd> <t_ff>
```

- `p_deg`：**连续坐标**（不是 0~360 单圈）；`mit_here` 自动取当前连续角。
- `v_rpm`：目标速度（输出 rpm）；`kp` [Nm/rad]、`kd` [Nm/(rad/s)]、`t_ff` [Nm]。
- 要求：`control.mit.enable=1`（默认已开）、已标定、FOC READY/RUN、连续角可信、
  `|p_des − 当前角| ≤ 90°`。
- `kp`/`kd` 超过 `mit.kp_max/kd_max` 会被**拒绝**（CAN 入口则是钳位）。

### 7.4 摩擦前馈

```text
motion friction show
motion friction cal [tol_pct]   # job，约 26 s；需已 motion cal、motion idle、FOC READY/RUN
motion friction apply           # RAM
motion friction save            # flash（FOC OFF）
```

### 7.5 `motion show` 关键行

```text
motion: mode cal zero valid angle active calibrating samples reason
shaft/speed/phase/torque/iq/good/bad
exec: iq_acc hold zoh lim sat
position: sp vel_cmd err
travel: pos sp valid rebase
diag: now cur tgt vff gain
mit: kp kd err tq sat gsat eclamp
reject: sample duplicate vernier timing margin
```

---

## 8. 标定

### 8.1 `cal current`

```text
cal current
```

电流零点标定（256 帧）。要求电机停止、无故障、FOC OFF。以 job 运行。

### 8.2 `cal encoder [apply|save|show]`

```text
cal encoder            # 扫描/验证电角度零点（自由轴，约 26 s）
cal encoder apply      # 应用结果到 RAM
cal encoder save       # 写 flash FENC（FOC OFF）
cal encoder show       # runtime/params/valid 与最近结果
```

- 无参运行：FOC READY/RUN，转子**自由**（不装负载）；2 A d 轴 + 自适应 2/4 A q 轴探针。
- `apply` 后零点/方向写 RAM；`save` 需先 `apply`，且 FOC OFF。
- 零点名义值兜底见 `motor.encoder.electrical_offset_rad`；不要用 0 代替。

### 8.3 `cal rl [show|apply|save|clear]`

```text
cal rl            # Rs/Ld + PI 自整定（FOC READY；需先 cal encoder）
cal rl show       # 结果、阶梯落点、retained PASS
cal rl apply      # 应用 kp/ki（FOC OFF）
cal rl save       # 写 flash FCUR
cal rl clear      # 清 FCUR 与 RAM 结果
```

### 8.4 `cal inertia [max_current_a|show|apply|save]`

```text
cal inertia            # 默认最大 2 A；多点正反扫
cal inertia <A>        # 指定最大电流
cal inertia show
cal inertia apply      # 写 motor.load_inertia_kgm2（FOC OFF）
cal inertia save       # 写 MPRM
```

要求自由轴、FOC READY/RUN；`storage: reset` 时 save 的结果下次启动会被 YAML 覆盖。

### 8.5 `cal cogging [speed_rad_s] [revs]|show|apply|save|clear`

```text
cal cogging                    # 默认多点 0.5/1.0/2.0/3.0 rad/s，约 100 s；
                               # 需自由轴、motion idle、FOC READY/RUN
cal cogging <speed_rad_s> <revs>   # 单点快速检查
cal cogging show|apply|save|clear
```

`apply` 要求 FOC OFF；`save` 写独立角表扇区；`clear` 清表并擦扇区。

---

## 9. 齿槽补偿开关

```text
cogging [show]
cogging on|off
cogging rotor on|off
cogging output on|off
cogging gain <0..2>
```

- `on/rotor on/output on` 前必须停止运动。
- 默认由 `config/software.yaml` 的 `control.cogging.*` 决定（当前默认开启）。
- 角表有效但总开关关闭时，用 `cogging on` 打开。

---

## 10. 参数

```text
param list [motor|hardware|software]
param get <name>
param set <name> <value>
param reset [<name>|motor|hardware|software]
param save
```

### 10.1 名称与单位

- 名称空间：`motor.*`、`hardware.*`、`software.*`；`param list` 会打印值、单位和 apply 类型。
- 布尔字段用 `0/1`（YAML 里写 `true/false`）。
- 浮点直接写十进制；范围按生成器 schema，超限会拒绝。

### 10.2 apply 语义

| apply       | 含义                                        |
| :---------- | :------------------------------------------ |
| `live`    | set 立即生效（消费者实时读取）              |
| `foc_off` | set 仅 FOC OFF 接受，下一次 `foc on` 生效 |
| `reboot`  | set 只改 RAM，重启后生效                    |

### 10.3 持久化与 storage

- `param save`：FOC OFF 且无 stop pending；写 `motor` + `software` 两域到 flash（MPRM/SPRM），hardware 不持久化。
- `param reset`：FOC OFF；只改 RAM，**不写 flash**。`reset motor` 会清编码器标定标志。
- `storage`（`software.storage`，编译期）：`keep` 用 flash；`reset` 忽略 flash、SPRM/MPRM 回 YAML 并写回、清 FCUR；`override` 本次启动用 YAML 但不写回。
- 单改某一项（例如 MIT 增益）建议 `param set` + `param save`，不要整体 `reset software`。

---

## 11. 典型流程

### 11.1 首次装机 / 换编码器

```text
foc on
cal encoder              # 自由轴，约 26 s
cal encoder apply
foc off
cal encoder save

motion cal               # FOC OFF、静止
motion apply
motion zero save

foc on
cal rl                   # Rs/Ld + PI（需 FOC READY）
foc off                  # apply/save 只允许在 OFF
cal rl apply
cal rl save
```

### 11.2 日常启动

```text
status
foc on
motion speed 60          # 60 rpm 输出
motion off
foc off
```

### 11.3 MIT 调试

```text
foc on
motion mit_here 0 10 0.5 0     # 原地、低速阻尼、轻刚度
motion show                    # 看 mit: kp/kd/err/tq/sat
motion mit_here 0 20 1.0 0     # 逐步加刚度
motion off
foc off
```

### 11.4 故障恢复

```text
fault show
foc off              # FAULT -> OFF
fault clear
foc on
```

---

## 12. 注意事项

1. **终端单位是 deg/rpm/Nm**，CAN 是 rad/rad/s；不要混用。
2. **job 运行期间输入被丢弃**，不会按键中止；等待完成或超时。
3. **flash 写入期间不要断电/拔 USB**；`param save` 等命令会短暂停顿。
4. **`foc off` / `motion off` 是阻塞软停**，最长约 3 s。
5. **`motion off` 与 `foc off` 都会走到 OFF + 关 +12V**；`motion off` 额外先撤销运动输出，
   `foc off` 直接对 FOC 发起软停。高速时两者都可能需要数秒。
6. **`motor iq/id` 需要 DIRECT 电流源**；跑过 motion 后先 `motion off`，必要时 `foc off` + `motion direct`。
7. **`COMM_TIMEOUT` 不在 `fault policy/inject` 名称表**；通信看门狗只能由实际超时触发。
8. **`storage: keep` 时 flash 覆盖 YAML**；改参数后必须 `param save`，否则重启回旧值。
9. **MIT 默认开启**，但需要已标定、FOC READY/RUN、位置误差 ≤90°。
10. **`adc diag` 需连续两次、间隔 <8.9 s**（32 位 cycle 回绕）。
