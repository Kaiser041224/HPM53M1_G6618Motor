# ODrive 对标差距与推进路线（设计文档）

- 日期：2026-10-27
- 状态：待评审（尚未实施）
- 适用范围：HPM53M1_G6618Motor 固件（单轴，G66-18 + 50:1 减速器，24V 母线）
- 对标对象：ODrive 0.6.x 固件功能面 + CANSimple 协议

---

## 1. 意图与理由

本工程的控制内核已经完成并台架验证：30kHz 电流环、2kHz 带宽自整定、速度观测器、
位置环 + 梯形轨迹 + 惯量前馈、双角表齿槽补偿、死区补偿、boost 电流管理、保护链 v3
（分层判据 + 四级动作 + 硬件 WDOG 同拍 TRIP），并有 159 万 checks 的主机回归套件。

问题不在“控制算法不够好”，而在**这台驱动器还不能作为节点被整机使用**：

- `App/Comm/can/` 是空的（只有 `.gitkeep`），`config/software.yaml` 的 `can:` 四项
  自标“占位 / 待协议定稿 / 解码未实现”；
- 没有任何通信看门狗，一旦上总线，主机崩溃后电机会继续执行最后的 setpoint；
- 没有再生能量出口（无 ibus 采样、无制动电阻），高速减速只能靠母线 OV 停机；
- 没有失速/堵转判据，堵转时会稳定顶在电流上限直到热保护（而热保护也尚未接入）。

本文件把这些差距、方案和推进顺序写清楚，供决策“先做哪一批”。

### 1.1 与 ODrive 的定位差异（有意保留）

| 项 | ODrive | 本工程 | 决策 |
| :--- | :--- | :--- | :--- |
| 错误体系 | 扁平 bitmask + `disarm_reason` | policy 表 + WARN/DERATE/STOP/TRIP | **保留本工程**，更细 |
| 齿槽补偿 | 单表、单圈 | rotor 512 + output 256 双表 | **保留本工程** |
| 单位 | turns、turn/s | 内部 rad、用户坐标带 `output_direction` + 减速比 | 内部保留 rad；CAN 单位见 §4.1 |
| 轴数 | 双轴 Axis 数组 | 单轴 | 不做多轴抽象 |
| ibus 功率限 | `I_bus/P_bus` 软限 | 无 ibus 采样 | 用 §4.4 的电功率估算替代 |

---

## 2. 目标 / 非目标 / 反目标

### 2.1 目标

1. **可集成**：通过 CAN 被外部主站按 ≤1kHz 控制，并能安全失去通信。
2. **失控有兜底**：堵转、通信中断、母线回灌都有确定动作，而不是“顶在上限等热”。
3. **可调**：弱磁/再生抑制这类高速区策略有足够的观测手段，不靠盲调。
4. **能升级**：现场无需 J-Link 即可更新固件（P2）。

### 2.2 非目标

- 多轴 / 主从同步（MIRROR）——单电机产品。
- sensorless 无感运行——编码器是既定方案。
- 完整 ODrive 参数体系（Fibre/odrivetool/SDO）——调参走自研 USB/UART，CAN 只做控制与状态。
- 硬件改板（本次不改 PCB；NTC 开路与无制动电阻按现状处理）。

### 2.3 反目标（即使功能成立也算失败）

- 为了接总线而降低控制环时限（FOC ISR 不得因通信变长）。
- 为了弱磁而放宽 `duty_max` 或保护阈值。
- 新增静态缓冲导致 DLM/AHB 超过 95%（当前 88.80% / 90.92%，余量本就紧张）。
- 把“WARN 不阻塞使能”这类已验证语义改回去。

---

## 3. 约束（已确认的事实）

| 约束 | 值 / 依据 |
| :--- | :--- |
| DLM 占用 | 113 KB / 130304 B = **88.80%**（`build/last_build.log`） |
| AHB_SRAM 占用 | 29792 B / 32 KB = **90.92%** |
| ILM 占用 | 32776 B / 128 KB = 25.01% |
| FLASH 占用 | 259488 B / 1012 KB = 25.04% |
| 无 ibus 采样 | `hardware.yaml` 无该项；`app_analog_signal` 只做 vbus/NTC/CANID |
| 无制动通路上报 | 用户确认：无 dump 电阻及预留 → §4.4 只做软件软限 |
| 母线快通道 | 30kHz、与电流同源触发、`app_adc_get_vbus_fast()`，已定案不可退回慢通道 |
| 保护动作层 | policy 表 + `fault.actions_enable`（默认 false）+ 运行期 `fault protect on/off` |
| 现有 fault 码 | bit0..10 已占（`APP_FAULT_CODE_COUNT = 11`），新增从 bit11 起 |
| 控制所有权 | 已有 `APP_MOTION_CMD_OWNER_DENIED`（校准/vtest 占用），尚无 CAN/USB 仲裁 |
| CAN 节点 ID | **ADC 电阻网络**（`ADC1_IN15`/PB00），仅返回原始电压；16 档解码未实现 |
| 慢节拍 | 1kHz `app_io_step`：`app_fault_process/tick/wdog_service` |

---

## 4. 差距与方案

### 4.1 CAN 协议 v1（P0）

**现状事实**：`App/Platform/Src/app_can.c`（475 行）只提供 send/receive/过滤器/统计；
`App/Comm/can/{Inc,Src}` 为空；`software.yaml` 的 `can:` 段全部是占位值。

**方案**：采用 CANSimple 的 **ID 布局与消息语义**，单位改用 SI。

- ID 布局：`(node_id << 5) | cmd_id`，标准 11-bit；`node_id ∈ [1,15]`，16..62 保留。
- 波特率：1 Mbps（`software.yaml` 已有）。
- 节点 ID 来源（**已核实，此前文档写错**）：不是 GPIO，而是**电阻网络的 ADC 采样**——
  `ADC_CH_V_CANID` = ADC1_IN15 / PB00，`app_analog_signal.c:137` 只返回原始电压并标注
  “解码表待补充”，`hardware.yaml` 仅给出 `canid_dip.levels: 16`。
  - **v1 决策**：不实现 DIP 解码，节点号取 `can.node_id`（YAML 配置 + `param` 可写，
    默认 1）；DIP 16 档阈值表作为独立后续任务（需要逐档电压实测）。
  - 理由：16 档阈值表是硬件测量任务，会阻塞整个批次 1；配置节点号已能满足单机/小批量。

**单位决策（用户已定，2026-10-27）**：**对外（CAN / 终端 / 上位机）统一 `deg` 与 `rpm`**
（速度按**出轴** rpm），转矩用 `Nm`；仅"确实需要角度微分量"的参数保留 `rad` 并显式标注。

- 内部算法仍全 `rad` / `rad/s`（不改 FOC、观测器、轨迹）。
- 换算点集中在协议层与终端解析层：`rpm → rad/s` 用 `2π/60 / reducer_ratio`；
  `deg ↔ rad` 用现有 `APP_TERMINAL_FOC_RAD_PER_DEG` 系列宏。
- 与终端的现有语义一致：`motion speed` 已经是 **rpm 输入**（
  `app_terminal_cmd_foc.c:727`），`motion pos` 已经是 **deg 输入**；本次统一的是
  **显示侧**（`foc`/`motion show`/`cal` 输出仍大量用 rad）。
- 不使用 ODrive 的 turns：turns 会在"转子圈 / 出轴圈(50:1) / 用户方向"之间产生
  三重换算，且与现有终端语义冲突。

**一期消息集（12 条）**：

| cmd | 方向 | 载荷 | 映射到 |
| :--- | :--- | :--- | :--- |
| 0x001 Heartbeat | 设备→主机 | u32 errors, u8 state, u8 proc_result, u8 traj_done, u8 rsv | `fault` + `foc`/`motion` 快照；周期 100ms |
| 0x002 Estop | 主机→设备 | 空 | 立即 `emergency_stop()`（force_low + 关 12V） |
| 0x003 Get_Error | 设备→主机 | u32 active, u32 latched | `fault show` 的 active/latched |
| 0x007 Set_Axis_State | 主机→设备 | u8 requested_state | 0=IDLE(`foc off`)、8=CLOSED_LOOP(`foc on`) |
| 0x009 Get_Encoder_Estimates | 设备→主机 | f32 pos_deg, f32 vel_rpm | `motion` 快照；**默认 1kHz**（见总线负载核算） |
| 0x00b Set_Controller_Mode | 主机→设备 | u8 control_mode, u8 input_mode | torque/speed/position |
| 0x00c Set_Input_Pos | 主机→设备 | f32 pos_deg, i16 vel_ff(0.001 rpm), i16 tq_ff(0.001 Nm) | `app_motion_position()`（内部转 rad） |
| 0x00d Set_Input_Vel | 主机→设备 | f32 vel_rpm, i16 tq_ff(0.001 Nm) | `app_motion_speed()` |
| 0x00e Set_Input_Torque | 主机→设备 | f32 torque_nm | `app_motion_torque()` |
| 0x00f Set_Limits | 主机→设备 | f32 vel_limit_rpm, f32 current_limit_a | `limits.*`（**只降不升**、不持久化，见下） |
| 0x018 Clear_Errors | 主机→设备 | u32 identify | `fault clear`（identify=0 清全部；否则须匹配 `first`） |
| 0x019 Set_Absolute_Position | 主机→设备 | f32 pos | `motion zero` 到指定值（可选，二期） |

**总线负载核算（1 kHz 回报的前提）**

CAN 标准帧 8 字节约 111 bit，计入最坏位填充取 **≈125 bit** → 1 Mbps 下单帧 ≈125 µs：

| 配置 | 往返帧率 | 负载 | 判断 |
| :--- | :--- | :--- | :--- |
| 1 节点，控制+回报各 1 kHz | 2 kHz | ≈25% | 可行 |
| 2 节点，同上 | 4 kHz | ≈50% | 可行（上限） |
| 4 节点，同上 | 8 kHz | ≈100% | **不可行** |
| 4 节点，回报 250 Hz | 5 kHz | ≈62% | 可行 |

结论：**1 kHz 双向回报仅适用于 1~2 节点**。≥4 节点必须把
`encoder_msg_rate` 降到 250~500 Hz，或改为按需 RTR 请求。该公式写入协议文档，
出厂默认取 1 kHz（用户已定），并在配置说明中标注节点数上限。

**`Set_Limits` 语义（builder check Q10）**：

- **只降不升**：主机只能把 `vel_limit` / `current_limit` 降到**当前生效值以下**，
  不能超过 YAML 配置。这是安全夹紧，不是“主机可任意改参数”。
- **不持久化**：重启或 `param reset` 回到 YAML 值。
- 实现前提：`limits.motion_speed_limit_rad_s` 目前是在 `app_motion_init()` 被 latch
  到静态变量的（`app_motion.c:319`），**没有运行期入口** → 需新增
  `app_motion_set_speed_limit()`；`i_q_max_a` 已有 live 路径，可直接复用。

**错误码编码（builder check Q7）**：

- Heartbeat 的 `errors` 与 `Get_Error` 的 `active/latched` 使用**本地位掩码**
  （bit0..14，随 §4.2/§4.3/§4.8 扩展），**不采用 ODrive 的错误位编号**。
- 理由：故障集合根本不同（本工程有分相 WDOG、无 sensorless 估计器），强行对齐会
  让上位机误读。协议文档必须附完整码位表。
- 上位机已确定为**自研 USB/UART**（§6），因此**不需要 ODrive 错误码翻译层**；
  本地位掩码直接进 Heartbeat / Get_Error。

**CAN 的职责边界（用户已定）**：CAN 只承载**控制与状态**，不做 SDO、不做参数树。
参数读写、标定、波形上传统一走 USB terminal / UART。这使批次 1 的协议面缩小到 12 条报文。

**位置语义（v1 明确限制）**：`Set_Input_Pos` 接受任意**有限**值，内部用
`foc_wrap_2pi()` 折叠到单圈（等价终端 `motion pos` 的 360° 折叠），**负值合法**。
多圈行程不支持，需 §4.7；协议文档与 Heartbeat 的 `proc_result` 必须能表达
“超出单圈能力”这一拒绝原因，不得静默 wrap。

**执行模型（必须在一期定死，否则看门狗会与停机互相打死）**

台架已存在的约束：`foc off` 会调用 `app_foc_wait_off(3000)` 在 **1kHz io 任务内自旋
最长 3s**（`app_foc.c:1076-1090`，期间只泵 `app_fault_tick()` / `app_fault_wdog_service()`）。
而 `app_can_poll()` 同样由 io 任务调用（`app_logic.c:363`）。若协议处理也放在这条链上，
一次 `Set_Axis_State=0` 就会让 CAN RX 静默 3s → 通信看门狗超时 → 在**受控停机过程中**
升级为 TRIP（关桥）。因此规定：

| 环节 | 位置 | 允许做的事 |
| :--- | :--- | :--- |
| CAN RX | 中断 / 回调 | **只入队 + 刷新"最后有效帧"时间戳**；零协议逻辑、零浮点运算 |
| 协议解析 | 1kHz io 任务 | 解析、校验、把命令**投递**给目标模块 |
| 状态转换 | 委托 | `Set_Axis_State` / `Estop` / `COMM_TIMEOUT` 一律**非阻塞请求**，由 `app_foc_run_once` 消费；**禁止**在协议 handler 内同步调用 `foc off` 或 `app_foc_wait_off()` |
| 主动停机窗口 | — | 自旋期间**挂起通信看门狗**（见 §4.2），否则“有意的静默”会被判为“通信丢失” |

**停机窗口的队列语义（不新增任务、不改已验证的软关断）**

上面的表已经隐含一个事实：`foc off` 自旋的 3s 内，io 任务不会跑协议解析，
RX 环形队列一定会溢出（1 kHz 主机 × 3s = 3000 帧）。这**不需要**修 `app_foc_wait_off`
（软关断顺序已台架验证，AGENTS 明令不得回退），而是规定：

1. **溢出即丢帧，可接受**：setpoint 是幂等的，丢帧只影响时延，不影响正确性。
2. **必须有溢出计数**（`can_rx_dropped`）并在 Heartbeat/诊断里可见，
   静默丢弃不可接受。
3. **停机完成或状态转为 OFF 时清空 RX 队列**：否则恢复瞬间会执行 3 秒前的陈旧
   setpoint（虽然 motion 层会以 `FOC_NOT_READY` 拒绝，但不应依赖下游兜底）。
4. **兜底门禁**：FOC 非 READY/RUN 时，所有 `Set_Input_*` 返回拒绝原因，
   不缓存、不延后执行。

因此“喂狗在 ISR、解析在 io、溢出可丢、恢复清队”四件事合起来，既避免新增
FreeRTOS 任务（heap 紧张），也不触碰已验证的停机时序。

**`app_debug_can.c` 的归属（builder check Q11）**：该模块当前持有 RX 回调、两条 filter
与 1Hz `tx_report_id` 报文，且仅在调试自检路径初始化。一期必须**移交所有权**：
协议模块接管 RX 回调与 filter，`app_debug_can` 降级为只读统计/dump，不再发周期性报文。
否则生产总线上会出现两个 RX 所有者与多余的 0x181 帧。

**必须一并解决的设计点：控制权仲裁（owner）**

CAN 与 USB 终端都能下发 setpoint。若不定义所有权，两边会互相覆盖，看门狗也会
被调试命令意外喂饱。方案：

- 新增 `app_motion` 的 owner 概念：`NONE / USB / CAN / CALIB`。
- 谁成功下发命令谁取得 owner；其他源下发时：
  - owner=CALIB → 拒绝（已有语义）；
  - owner=另一通信源且 motion 正在 `active` → 返回 `OWNER_DENIED`，除非显式 `motion take` 夺取。
- `foc off` / `motion off` 释放 owner。

**实现位置**：`App/Comm/can/Src/app_can_protocol.c`（新），只依赖 `app_motion` /
`app_foc` / `app_fault` / `app_param`；`app_can_*` 平台层不改。

**验收**：
1. 主机脚本 1kHz 发 `Set_Input_Vel`，`omega` 跟随，`profiler dump` 显示 FOC ISR
   耗时无上升（对比基线 14.5µs / 16.6µs）。
2. 拔掉 CAN 线 → `comm_timeout` 内进入受控停机，`fault show` 有 `COMM_TIMEOUT`。
3. USB 与 CAN 同时下发 → 后者被拒且原因可见。

---

### 4.2 通信看门狗（P0，必须与 4.1 同批）

**现状事实**：`App/` 内无控制侧 watchdog/heartbeat/timeout 逻辑（仅 RTT/RTOS 与
cogging 标定的局部看门狗）。

**出厂默认 = `false` 的后果（用户已定，2026-10-27；不得含糊）**

- `software.yaml:26` 的 `fault.actions_enable` **保持 `false`**（仅警告）。
- 这意味着出厂配置下，**包括 `COMM_TIMEOUT` 在内的所有故障都只告警、不停机**。
  “通信丢失自动停机”这一安全能力**在出厂状态下不生效**。
- 因此：
  - 文档与协议**不得承诺**“默认即安全停机”；
  - 整机集成时 `actions_enable`（或运行期 `fault protect on`）必须作为**必检项**；
    终端已能直接看到 `actions=OFF(仅警告)`，无需额外工具。
  - 所有 P0 验收流程显式前置 `fault protect on`，不依赖默认值。

**方案**：

- 新增 `APP_FAULT_COMM_TIMEOUT`（bit11），policy 设计值 **STOP**。
- 参数：`fault.comm_timeout_ms`（默认 100，0=禁用）。
- 喂狗源可配：`fault.comm_source` ∈ {`can`, `usb`, `both`}，默认 `can`。
  - 理由：终端调试时不应阻止 CAN 看门狗生效，否则总线丢失会被调试命令掩盖。
  - `usb` 模式下终端命令也算喂狗（台架联调用），**不作为出厂默认**。

**三条此前未定义的语义（builder check Q3，此处定案）**：

1. **何时 arm**：**首次收到有效控制帧后**才 arm，boot 后不 arm。
   - 否则独立台架（没接主机）在 `actions_enable=true` 时会自己停机，
     所有标定/调试流程都走不通。
2. **什么算喂狗**：只算**被接受的控制帧** —— `Set_Input_Pos/Vel/Torque`、
   `Set_Axis_State`、`Set_Limits`、`Set_Controller_Mode`。
   `Get_*` 查询与 RTR 请求**不算**（否则一个“还在发心跳但已停止发命令”的主机
   会让电机继续跑，正好是看门狗要防的场景）。
   - 喂狗动作发生在 RX 中断侧的“有效帧时间戳”，不依赖 1kHz 协议解析是否被调度。
3. **何时挂起**：
   - `CALIB`（`cal encoder` ≈26s、`cal cogging` ≈100s、`cal rl`、`cal inertia`）期间 **suspend**，
     **不升级为 TRIP**；
   - 主动停机窗口（`app_foc_wait_off` 的 3s 自旋、STOP deadline 等待）期间 **suspend**；
   - 其余情况超时才按 policy 动作（STOP → 无法执行则升 TRIP）。

**验收**：`fault protect on` + `fault inject COMM_TIMEOUT on` 走 STOP；
真实拔线走同一条路径；`fault show` 的 `first` 记录该码；
`cal cogging` 全程不被看门狗打断。

---

### 4.3 失速 / 堵转判据（P0）

**现状事实**：无 `stall|spinout` 判据；只有电流限幅与速度限幅。堵转时电流稳定
顶在 `i_limit`（30A 或 DERATE 后 15A），直到热问题出现——而热保护尚未接入。

**方案**：新增 `APP_FAULT_STALL`（bit12），policy 设计值 **STOP**。判据在 1kHz
慢保护（不占快环）：

```text
mode ∈ {POSITION, SPEED} && motion.active
&& |vel_cmd − vel_fb| > stall_vel_rad_s        // 默认 1.0 rad/s（轴端）
&& |i_q_ref| ≥ stall_current_ratio × i_limit   // 默认 0.9
持续 stall_ms                                   // 默认 300
```

- `vel_cmd`：速度模式取斜坡后目标；位置模式取 `traj_vel_rad_s`。
- **TORQUE 模式不判失速**（堵转是合法短时工况）。
- **但力矩模式不能无限过载**（builder check Q1 子问题）：现有 `OC_SLOW` 只把电流降到
  DERATE 后的 15A，boost 的 I²t 预算耗尽也只回落到连续 24.3A —— 两者都可无限持续，
  而热保护属 P1 且被 NTC 选型阻塞。因此一期补一条**长时过载兜底**：

```text
OC_SLOW 的 DERATE 生效持续 > slow_derate_to_stop_s（默认 10s）
→ APP_FAULT_STALL（同一码，reason 区分）→ policy STOP
```

  这是不需要温度传感器的近似热模型，代价是“长时间大电流”的合法工况也会被停。
  若用户的工艺确实需要 >10s 连续大电流，应改 `i_q_continuous_a`，而不是放宽该时限。
- 参数进 `software.yaml` 的 `fault.stall_*` 与 `fault.slow_derate_to_stop_s`。
- 反目标：不得用“电流超限”单独判定失速（与 OC_SLOW 重复且会误伤加速段）。

**验收**：自由轴 `motion speed 30` 正常运行 60s 不误报；机械夹持输出轴后
300ms 内 `STALL` 出现并受控停机；解除后 `fault clear` 可恢复。

---

### 4.4 再生功率软限（P0，纯软件）

**现状事实**：无 ibus 采样；无制动电阻；`VBUS_OV` 一级 STOP（卸转矩）、二级 40V TRIP。
**这是工程推断，需台架确认**：`motion speed 30` 急减速时机械能全部回灌母线，
软件只能被动等 vbus 回落，结果通常是 OV STOP —— 保护成功但任务失败。

**方案**：用 FOC 内部量估算电功率，不需要新增采样。

```text
P_elec ≈ 1.5 × (v_d·i_d + v_q·i_q)     // W；正 = 从母线吸收，负 = 回灌（估算，忽略损耗）
```

**四个此前含糊、现在定死的点（builder check Q5）**：

1. **插入点**：`app_motion` 的转矩提交之前（与 `torque_nm` 快照同域，即内部物理坐标、
   应用 `output_direction` 之前）。**不在** `app_foc_current` 的 i_q 预算里做 ——
   那里已是电气域，再换算回转矩会引入 `kt` 与减速比的双重误差。
   因此 **TORQUE 模式（主机直接下发制动转矩）同样被覆盖**，这是与死区补偿不同的一点。
2. **电压口径**：用**实际施加**的 dq 电压（经 `v_scale` 与相电压钳位之后），
   不要用 PI 输出。饱和时 PI 输出会高估 |P|，而饱和正是限幅器最需要准确的时刻。
   数据源：`app_foc_current.h` 的 `v_d_v/v_q_v` 已存在；需确认其为施加值，
   若为 PI 输出则新增一个"施加值"字段（这是实施期第一个要核对的事实）。
3. **制动判定与符号**（避免“制动时反而加转矩”）：

```text
P_regen    = max(0, −P_elec)                      // W，回灌功率
T_brake_max = P_max / max(|ω_out|, ω_min)          // Nm，ω_min 默认 1 rad/s
if (T_cmd × ω_out < 0)                             // 仅制动方向
    T_cmd = sign(T_cmd) × min(|T_cmd|, T_brake_max)
```

   `ω_out` 用**轴端速度**（与 `T_cmd` 同域），不要用电角速度。
4. **anti-windup**：限幅后必须把“实际提交转矩”回算给速度环积分（用限幅前请求 −
   实际提交的差），否则减速段积分饱和，解除限幅时会出现转矩过冲。

- 叠加 vbus 上升率项：`dv/dt > rise_th` 时进一步收紧 `T_brake_max`
  （数据源用 30kHz 快通道，已有）。
- 参数：`control.regen.p_max_w`（默认 150，0=关闭）、`vbus_rise_v_per_s`（默认 20）、
  `vbus_margin_v`（默认 2，从 OV 阈值回退）。
- CALIB / vtest 期间不生效（与死区补偿同规则）。

**已知代价**：减速时间变长。这是替代“OV 停机”的显式取舍，不做隐藏。

**验收（A/B）**：同固件同热态，`motion speed 30` → `motion speed 0`，
记录 vbus 峰值与减速时间；关闭/开启各 3 轮，要求开启后 vbus 峰值 < 36V
（不触发 OV STOP）且无 TRIP。

---

### 4.5 触发式波形捕获（P1，工具）

**现状事实**：只有 RTT printf、`profiler dump`、终端快照。调弱磁/再生这类
ms 级时序只能靠反复打印，效率低，且容易重演“单臂观测误归因”。

**方案**：新增 `App/Debug/app_debug_scope.c`。

- 通道：最多 8 个，取自枚举（`i_d/i_q/v_d/v_q/vbus/vel_cmd/vel_fb/pos_err/torque_cmd/i_limit`）。
- 定标：每通道 float→int16（scale/offset），带宽减半。
- **采样率四档**：30 kHz（÷1）/ 6 kHz（÷5）/ 3 kHz（÷10）/ 1 kHz（÷30）。
  - 修正理由（builder check Q8）：2 kHz 带宽电流环的阶跃上升沿约 80 µs，
    1 kHz 只有 1~3 个点，**无法看清超调**。必须支持 30 kHz 档。
- **深度按档位限定，总量恒为 4 KB 上限**：

| 模式 | 通道 | 帧数 | 采样率 | 时间跨度 | 缓冲 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| A 阶跃 | 4 | 256 | 30 kHz | 8.5 ms | 2 KB |
| B 中速 | 8 | 256 | 6 kHz | 42 ms | 4 KB |
| C 慢过程 | 4 | 512 | 1 kHz | 512 ms | 4 KB |

- **触发与门控**：仅 `armed` 时采样；ISR 内只做“读快照 → 定标 → 写缓冲”，
  不做浮点重算。若采样期间 `g_foc_isr_overruns` 增加，**立即自动 disarm**
  （scope 绝不允许成为诱发 FOC 降级的元凶）。
- 上传：终端 `scope dump` 分块（每 1kHz tick ≤200µs，走 job 框架）；PC 侧脚本解析。

**内存核算（关键约束）**：DLM 实测 88.80%，余量 ≈14.5 KB。4 KB 缓冲后约 92%。
若后续要 8 KB，必须先把冷数据（`s_trace`、`s_rms_buf`）迁到 AHB 或复用。
注意：现有 `scripts/tests/layout/check_layout.sh` **只断言 ILM 符号驻留，没有内存
百分比守卫** —— “加守卫上限”是一项**新工具工作**，不是改一行断言。

**验收**：模式 A 能看清 `cal rl` PI 阶段的上升沿与超调（≥20 点/上升沿）；
模式 C 能覆盖一次弱磁进入/退出全过程；`scope dump` 期间 `adc diag` 的 `rate`
不偏离 30 kHz ±20%，且 `g_foc_isr_overruns` 不增加。

---

### 4.6 弱磁（P1，能力）

**现状事实**：`motion speed 30` 实测 `use=100% sat=1`，`i_d avg=-0.637 /
now=-4.046A`（负 d 是反弱磁方向，属耦合结果而非策略）。

**方案**：电压裕度闭环 + 解析前馈。

```text
V_max = duty_max × vbus / √3            // 已有 vmax
e_v   = use − use_target                // use_target 默认 0.95
i_d_ref = clamp(−(Kp·e_v + Ki∫e_v), −i_d_max, 0)
```

- 仅在 `|ωe| > we_enter` 时启用（默认 2000 rad/s 电角），避免低速无谓铜损。
- 电流预算：现有 `foc_current` **已是 Id 优先 2-norm**（先给 d 预算、q 用剩余），
  所以只要把 `i_d_ref` 纳入同一预算就会自动生效，**不需要新增优先级逻辑**
  （builder check 指出原稿这句是描述现状，已更正）。
  需要定的是：弱磁 `i_d` 是否计入 boost 的 I²t 预算 —— 建议**不计入**，
  因为弱磁是稳态需求而非瞬态过载，计入会让高速持续运行时 boost 预算被缓慢耗尽。
- 退饱和：`use > 1.0` 持续 → 主动降 `i_q` 上限（保磁通优于保转矩）。
- 保护：`i_d_fw_max` 默认 0.3 × `i_q_continuous`；超限不叠加 boost 预算。

**依赖**：必须先有 §4.5 波形，否则无法确认退饱和时序。

**验收**：`motion speed 30`（轴端 30 rad/s）下 `use < 100%`、`sat=0`；
转矩能力下降曲线与理论 `V_max/(ωe·Ld)` 趋势一致；退出弱磁无过冲。

---

### 4.7 多圈位置 / circular setpoints（P1）

**现状事实**：`app_motion.c:542` 的 `over_angle` 拒绝 |target|>2π；
`app_motion_position()` 注释明确“单圈 0~2π”。而游标已能解 `s_nearest_turn`
（`:791`），多圈信息实际存在但被命令层裁掉。

**方案**：

- 新增连续位置状态 `pos_continuous_rad`（不 wrap），零点/方向变更时重基准。
- 两种模式：
  - `multi_turn`：目标为绝对值，累积多圈；
  - `circular`：目标 ∈ [0,2π)，按最短路径选圈（ODrive `circular_setpoints` 语义）。
- 默认保持现有单圈行为，新模式需显式开启（兼容既有台架流程）。
- 安全联锁：`angle_valid=0`（游标失配降级）时拒绝多圈命令，避免跳变。

**绝对性边界（builder check Q12，必须写进用户手册）**：

- 游标只能在**有限圈数**内可靠分辨，且**断电期间的位移无法被记录**。因此：
  - `pos_continuous_rad` 仅在**同一上电周期内**保证连续；
  - 跨上电只能由游标恢复“最近圈”，**不保证绝对圈数**；
  - 需要绝对多圈定位 → 必须做回零（P2 的 §4.9 homing），或在安装规程里声明
    “断电不得移动超过 N 圈”。
- **前置测量任务**：入档前必须实测本装配游标能可靠分辨的最大出轴圈数
  （`reducer_ratio × (1 − output_ratio) ≈ 1` 决定分辨率，但实际受噪声/装配限制），
  该数值写入协议文档，不得凭公式宣称。

**验收**：`motion pos_multi` 连续两圈无跳变；断电移动 < 0.5 圈后恢复误差 < 0.01 rad；
失配注入时多圈命令被拒且给出原因；实测得到的最大可靠圈数记录在案。

---

### 4.8 NTC 温度与热降额（P1，存在硬件阻塞）

**现状事实**：`app_analog_signal.c:105-107` 只换算到电阻 `r_ntc0_ohm/r_ntc1_ohm`；
无 β→温度；`hardware.yaml` 的 `r25_ohm`/`b_value_k` 标注“占位，待选型修正”；
`adc diag` 显示 `ch1=65535`（NTC1 开路，硬件待查）。

**方案**：

```text
T = 1 / (1/T25 + ln(R/R25) / B) − 273.15
```

- **通道语义（已核实，原稿未定义）**：代码里没有 motor/board 标签 ——
  `ADC_CH_NTC0` = ADC1_IN11 / PB08，`ADC_CH_NTC1` = ADC1_IN1 / PB09；
  **NTC1 正是 `adc diag ch1=65535` 的开路那路**。
  - 新增 `hardware.ntc.role`（`ntc0=motor | ntc1=motor`），默认 `ntc0=motor`；
    输出统一为 `motor_temp_c` / `board_temp_c`，不把标签写死在代码里。
  - 若实测电机 NTC 恰是开路的那一路，则本项**只能先作用于板级**，
    §4.8 的电机热降额验收被硬件阻塞，需在文档中如实降级。
- 新增 `APP_FAULT_OVERTEMP`（bit13）：`ot_derate_c`（默认 100）→ DERATE；
  `ot_trip_c`（默认 120）→ **TRIP（锁存）**，不是 STOP。
  理由（builder check Q9）：STOP 不锁存、不阻塞使能，高温下可被立即重新使能 →
  形成“停机/使能”循环。过温应要求**冷却 + `fault clear`** 才能恢复。
- 新增 `APP_FAULT_TEMP_SENSOR`（bit14）：R > `max_ohm`（开路）或 R < 100Ω（短路）
  → WARN（单传感器失效不应停机，但必须可见）。
- **阻塞项**：NTC 型号选型 + NTC1 开路定位。软件可在选型前实现，但参数保持占位，
  不得用占位值判定过温。

**验收**：热风枪加热**有效通道** → 温度读数单调、与红外测温差 <5°C；
`fault inject OVERTEMP on` 走 DERATE；`OVERTEMP` 的 trip 级需 `fault clear` 才恢复
（验证锁存语义）；开路时出 `TEMP_SENSOR` 且不误报过温。

---

### 4.9 P2 工程化清单

| 项 | 现状 | 方案要点 | 前置 |
| :--- | :--- | :--- | :--- |
| CI | `scripts/tests/all.sh` 可跑，无 CI 配置 | 接入 CI 跑全量门禁；本地 pre-push 钩子 | 无 |
| DFU / 升级 | 无 `dfu/bootloader/iap` 引用 | 先确认 HPM SDK 的 bootloader 支持与 flash 布局冲突 | 需评估 |
| 端位 / 回零 | 无 | GPIO 端位 + `HOMING` 状态 | §4.7 多圈 |
| EWDG | 未做（已记账） | 先确认 HPM53M1 是否具备 EWDG | 需查手册 |
| 文档债 | `README.md` 仍是模板文案；`Doc/` 与 `docs/` 并存 | 重写 README，合并目录 | 无 |
| 参数体系 | `storage` 编译期；`reset` 会覆盖 `cal inertia save` | 运行期分组件 save/erase 语义 | 无 |

---

## 5. 推进顺序

```text
批次 1（P0：可集成 + 安全兜底）
  1.1 CAN 协议 v1 + owner 仲裁           ─┐ 必须同批
  1.2 通信看门狗 COMM_TIMEOUT            ─┘
  1.3 失速检测 STALL                     （独立）
  1.4 再生功率软限                        （独立）

批次 2（P1：观测）
  2.1 触发式波形捕获 scope

批次 3（P1：能力）
  3.1 弱磁（依赖 2.1）
  3.2 多圈 / circular
  3.3 NTC 温度 + 热降额（受硬件选型阻塞）

批次 4（P2：工程化）
  4.1 CI → 4.2 DFU 评估 → 4.3 端位/回零 → 4.4 EWDG → 4.5 文档债
```

**排序理由**：

- 1.1 与 1.2 同批：看门狗单独上线无意义；协议单独上线则留下失控风险。
- 1.3 / 1.4 不依赖总线，可在协议评审期间并行推进，且都直接挂到已有 policy 表。
- 2.1 先于 3.1：弱磁的难点是退饱和时序，无波形只能盲调。
- 3.3 后置：`r25_ohm`/`b_value_k` 仍是占位，软件做在不确定硬件上会白做。

**粗估**（人日，非承诺）：1.1 ≈ 3、1.2 ≈ 1、1.3 ≈ 1、1.4 ≈ 2.5、2.1 ≈ 3.5、
3.1 ≈ 4、3.2 ≈ 2.5、3.3 ≈ 1（不含选型）、4.x 合计 ≈ 5。

---

## 6. 风险与开放问题

| # | 项 | 说明 | 处置 |
| :--- | :--- | :--- | :--- |
| R1 | 内存余量 | DLM 88.80% / AHB 90.92%；新增缓冲极易触顶 | 每项实施前核算，`check_layout.sh` 加守卫 |
| R2 | CAN 与 USB 争用 | 无 owner 会导致 setpoint 互相覆盖、看门狗被误喂 | §4.1 的 owner 仲裁为一期必做 |
| R3 | 看门狗误停机 | 台架调试时若 source=can，USB 操作会被停机 | `comm_source` 可配，默认 can |
| R4 | 再生软限影响节拍 | 抬转矩下限会延长减速，可能影响 3s 停机窗口 | 与 `APP_FOC_STOP_SPEED_WAIT_TICKS` 联合验证 |
| R5 | 弱磁参数敏感 | 退饱和振荡、参数漂移 | 先观测后闭环；`i_d_fw_max` 独立上限 |
| R6 | NTC 硬件 | 型号未定、NTC1 开路 | 实现与判据分离，占位值不参与判定 |
| R7 | CAN 单位选择 | SI（rad）vs ODrive（turns）影响上位机复用 | 已给建议，需用户确认 |

**已决策（2026-10-27，用户确认，直接约束实施）**

| 项 | 决定 | 落点 |
| :--- | :--- | :--- |
| `fault.actions_enable` | **保持 `false`**（出厂仅警告） | §4.2 的后果声明；验收一律前置 `fault protect on` |
| 单位 | 对外 **deg / rpm**，内部 rad；必要参数保留 rad | §4.1 单位决策 |
| 调试上位机 | **自研**，基于现有 USB terminal + UART（已有分支初步尝试） | CAN **不做 SDO / 参数树**；调参、标定、波形走 USB/UART |
| CAN | 控制+回报 **1 kHz**、1 Mbps | §4.1 负载核算 → **节点数上限 2** |
| 温度 | NTC 参数待填，**暂不完整实现** | §4.8 保持阻塞状态，只做接口与判据骨架 |
| builder check 4 项 | **当前阶段先收敛**，再继续方案讨论 | 本轮完成 |

**仍然开放（不阻塞下一步）**

1. **上位机协议形态**：现有 ASCII 终端是否保留为唯一入口？UART 与 USB 是否同一套命令？
   是否需要二进制帧（用于 1kHz 波形上传）？—— 决定 §4.5 的上传通道，不影响批次 1。
2. `slow_derate_to_stop_s = 10s` 是否与工艺冲突 —— 若冲突，改 `i_q_continuous_a`。
3. 节点数上限若确实 >2，回报率需从 1 kHz 降到 250~500 Hz（见 §4.1 核算表）。

---

## 7. 本次已自行作出的裁量（供复核，不需回答）

builder check 暴露的问题里，以下由 Mephisto 直接定案并已写入正文：

| # | 决定 | 出处 |
| :--- | :--- | :--- |
| 1 | CAN 状态转换一律**非阻塞**；RX 中断只入队+打时间戳；`foc off` 自旋期间看门狗挂起 | §4.1 执行模型 |
| 2 | 看门狗**首次收到有效控制帧才 arm**；只有“被接受的控制帧”算喂狗；CALIB/vtest 期间挂起 | §4.2 |
| 3 | `Set_Input_Pos` 接受任意有限值并按单圈 wrap（负值合法）；多圈留 §4.7 | §4.1 位置语义 |
| 4 | `Set_Limits` **只降不升、不持久化**；新增 `app_motion_set_speed_limit()` | §4.1 |
| 5 | 错误码用**本地位掩码**，不对齐 ODrive 编号 | §4.1 |
| 6 | 再生软限插在 `app_motion` 转矩提交前（覆盖 TORQUE 模式），用**施加电压**算功率，限幅后回算 anti-windup | §4.4 |
| 7 | TORQUE 模式补“DERATE 持续 >10s → STOP”的长时过载兜底 | §4.3 |
| 8 | scope 增加 30kHz 档（4ch×256 帧=2KB），并按 `g_foc_isr_overruns` 自动 disarm | §4.5 |
| 9 | 过温 trip 级用 **TRIP（锁存）** 而非 STOP，避免“高温重新使能”循环 | §4.8 |
| 10 | NTC 通道标签由配置决定，不写死；NTC1 开路的事实如实降级 | §4.8 |
| 11 | 多圈只保证同一上电周期内连续；跨上电需 homing 或安装规程约束 | §4.7 |
| 12 | v1 不做 DIP 解码，节点号取 `can.node_id` 配置项 | §4.1 |
| 13 | `app_debug_can` 移交 RX/filter 所有权，降级为只读诊断 | §4.1 |
| 14 | 对外单位 **deg / rpm**（内部 rad）；换算集中在协议层与终端解析层 | §4.1 |
| 15 | CAN 1 kHz 双向回报 → **节点数上限 2**；≥4 节点降到 250~500 Hz | §4.1 负载核算 |
| 16 | 停机窗口 RX 溢出**丢帧可接受**，但必须计数、恢复时清队、下游门禁兜底 | §4.1 队列语义 |
| 17 | CAN **不做 SDO/参数树**，调参/标定/波形走 USB/UART | §4.1 职责边界 |

---

## 8. 留给实施者的裁量

以下由实施者按现场证据决定，不必回到本文件：

- 各 `software.yaml` 参数的最终默认值与上下限。
- `scope` 的通道集合与定标系数。
- 弱磁 PI 的具体整定方法与初值。
- CAN 发送队列深度与丢弃策略。
- 单元测试的用例组织方式（沿用 `scripts/tests/<domain>/` 现有约定）。
---

## 9. 实施状态（2026-10-27，第一批：多圈 + MIT + CAN）

### 9.1 已实现

| 模块 | 文件 | 内容 |
| :--- | :--- | :--- |
| 纯算法 | `App/Algorithm/{Inc,Src}/algo_mit.*` | PD+前馈、增益斜坡、误差钳制、前馈占比、力矩限幅、饱和标记 |
| 纯算法 | `App/Algorithm/{Inc,Src}/algo_motion.*` | 新增 `algo_motion_continuous_step()`（连续角 unwrap / 跳变 / 重基准） |
| 控制 | `App/Control/Src/app_motion.c` | 连续角累积、`position_multi` / `position_rel`、MIT 模式、运行期限幅 setter |
| 协议 | `App/Comm/can/{Inc,Src}/app_can_protocol.*` | CANSimple ID 布局、12 条报文、MIT compact 位域 + dual 双帧（纯函数） |
| 服务 | `App/Comm/can/{Inc,Src}/app_can_service.*` | 队列、状态机、通信看门狗、1kHz 回报、dual 配对 |
| 平台 | `App/Platform/Src/app_can.c` | 新增 `app_can_set_rx_isr_hook()`（真 ISR 喂狗，不依赖任务调度） |
| 保护 | `App/Control/{Inc,Src}/app_fault.*` | 新增 `APP_FAULT_COMM_TIMEOUT`（bit11，设计动作 STOP） |
| 终端 | `app_terminal_cmd_foc.c` / `app_terminal_cmd_diag.c` | `motion pos_multi/pos_rel/mit`、`motion show` travel/mit 行、`can` 诊断 |

`app_debug_can` 已移交 RX 回调/过滤器所有权，降级为只读统计（不再发 0x181 周期报文）。

### 9.2 参数（`config/software.yaml`）

```yaml
control:
  position_loop:
    max_step_deg: 3600.0        # 单次命令最大位移（不可关闭）
    max_travel_deg: 36000.0     # 连续角绝对值上限（不可关闭）
    travel_min_deg: 0.0         # 可选软限位（0/0 = 关闭）
    travel_max_deg: 0.0
  mit:
    enable: false               # 需显式开启（param set 立即生效）
    kp_max: 50.0                # [Nm/rad]
    kd_max: 2.0                 # [Nm/(rad/s)]，保守值，待实测 observer 噪声后定
    gain_ramp_rate: 200.0       # [(Nm/rad)/s]
    pos_err_max_deg: 90.0
    t_ff_max_ratio: 1.0
    can_format: compact         # compact | dual（init 期消费）
    friction_ff_en: false
    inertia_ff_en: false
fault:
  comm_timeout_ms: 100          # CAN 看门狗（0 = 禁用）
  comm_wd_source: can           # can | usb | both
```

**多圈位置的必要条件**：`trajectory_vel_limit` 与 `trajectory_accel_limit` 均 > 0
（否则 `pos_multi/pos_rel` 与 CAN 位置命令被拒绝——大步命令必须有速度上限）。

### 9.3 测试步骤：USB 终端

```bash
# 0) 前提：多圈需要轨迹参数（APPLY_REBOOT → 改 YAML 后重烧）
#    trajectory_vel_limit: 30.0 / trajectory_accel_limit: 60.0
make artifacts && make flash-jlink

# 1) 多圈绝对 / 增量
foc on
motion show                 # 确认 travel: pos=... valid=1
motion pos_multi 720        # 出轴第 2 圈（绝对）
motion pos_rel -90          # 相对当前 -90°
motion pos_multi 99999      # 期望 TARGET_LIMIT（max_step 保护）

# 2) MIT（enable 可运行期打开）
param set software.control.mit.enable 1
foc on
motion mit_here 0 25 1 0    # ★ 在【当前位置】施加阻抗（p_des 自动取当前连续角）
motion mit 120 0 25 1 0     # 绝对坐标：p_des=120deg（与当前角误差需 ≤ pos_err_max_deg=90）
motion show                 # mit: kp→25（斜坡逼近） sat=0
motion mit_here 0 60 1 0    # 期望 TARGET_LIMIT（kp > kp_max=50）
motion mit 0 0 25 1 0       # 期望 TARGET_LIMIT（若当前角 >90deg：p_des=0 是绝对目标，不是"原地"）
motion off

# 3) CAN 诊断
can                         # node / wd / bus / proto 统计
```

> `fault.actions_enable` 出厂为 `false`：上面所有动作都只在 `fault protect on`
> 之后才会真正停机。做保护验收前先执行 `fault protect on`。

### 9.4 测试步骤：CAN（Linux `can-utils`，1 Mbps，节点 1）

ID = `(node_id << 5) | cmd`，节点 1 → 基址 `0x20`。

```bash
sudo ip link set can0 up type can bitrate 1000000

# 1) 上使能（Set_Axis_State = 0x27，8 = CLOSED_LOOP）
cansend can0 027#08

# 2) 位置模式（Set_Controller_Mode = 0x2B，control=3 POSITION，input=0）
cansend can0 02B#0300

# 3) 位置 90°（Set_Input_Pos = 0x2C，f32 deg + i16 vel_ff + i16 tq_ff）
#    f32 90.0 = 0x42B40000 → 小端 00 00 B4 42
cansend can0 02C#0000B44200000000

# 4) 速度模式 + 100 rpm（Set_Input_Vel = 0x2D；f32 100.0 = 0x42C80000）
cansend can0 02B#0200
cansend can0 02D#0000C8420000

# 5) 力矩 0.5 Nm（Set_Input_Torque = 0x2E；f32 0.5 = 0x3F000000）
cansend can0 02B#0100
cansend can0 02E#0000003F

# 6) MIT：模式 4 + compact（Set_Input_MIT = 0x30）
cansend can0 02B#0400
#    p=0(0.1deg) v=0(1rpm) kp=2048/4095≈0.5→25Nm/rad kd=2048→1.0 t_ff=0
cansend can0 030#0000000080000800

# 7) 限幅（Set_Limits = 0x2F，只降不升）
cansend can0 02F#0000FA440000A041     # 2000 rpm / 20 A

# 8) 通信看门狗：注入后停止发送，100ms 内应受控停机（需 fault protect on）
fault protect on            # USB 终端先开动作
#    然后停止 cansend，观察 fault show → COMM_TIMEOUT

# 9) 清故障（Clear_Errors = 0x38，identify=0）
cansend can0 038#00000000

# 10) 诊断
#     USB 终端执行 can：
#     proto.rx 应随控制帧增长；drop/bad 应为 0；mit.ok 在 dual 模式下增长
```

**回报帧**（设备→主机，可用 `candump` 观察）：

| ID | 内容 | 周期 |
| :--- | :--- | :--- |
| `0x21` | Heartbeat：u32 latched + u8 axis_state + u8 proc + u8 traj_done | 100 ms |
| `0x29` | Get_Encoder_Estimates：f32 pos_deg（连续）+ f32 vel_rpm | 1 kHz（1 Mbps 下建议 ≤2 节点） |

### 9.4.1 上板命令格式（`ID:数据`，节点 1）

```text
# 回报
21: u32 latched + u8 axis_state + u8 proc_result + u8 traj_done + u8 rsv   (100ms)
29: f32 pos_deg（连续） + f32 vel_rpm                                      (1kHz)

# 状态 / 模式 / 输入
27:08                         # CLOSED_LOOP（受控使能）
27:01                         # IDLE（受控停机：只发起请求，由 1kHz 状态机缓降+软关断）
2B:03 00                      # POSITION（先设模式，否则 Input 被忽略）
2C:00 00 C8 43 00 00 00 00    # 400deg（绝对连续坐标）
2D:00 00 C8 42 00 00          # +100 rpm
2E:00 00 00 3F                # +0.5 Nm
2F:00 00 FA 44 00 00 A0 41    # 2000rpm + 20A（只降不升）
30:00 00 00 00 80 00 08 00    # MIT compact：kp=0.5×kp_max, kd=0.5×kd_max
38:00 00 00 00                # 清故障
```

**`proc_result` 判读**（Heartbeat 第 6 字节）：`0=OK`，其余为 `-APP_MOTION_CMD_*`：

| 值 | 含义 | 值 | 含义 |
| :--- | :--- | :--- | :--- |
| 1 | 参数非法 | 6 | FOC 非 READY/RUN |
| 2 | 未标定/方向变更/连续角不可信 | 7 | 基准变更 |
| 3 | 反馈陈旧 | 8 | 模式锁定 |
| 4 | 目标超限（行程/幅值/增益/误差） | 9 | 生产者被拒（标定/vtest） |
| 5 | 增益未配置 | 10/11 | MIT 未启用 / 多圈需要轨迹 |

**IDLE 语义**：与终端 `foc off` 一致——只置受控停机请求，由 `app_foc_run_once`
推进「转矩归零 → 缓降 → 软关断（先关 +12V 再停 PWM）」。CAN handler **不得**
直接 `app_foc_disable()`（会跳过缓降，高速时等于突然断电）。

### 9.5 已知限制

1. **多圈绝对性**：连续角只在同一上电周期内可靠；断电后由游标恢复单圈，
   绝对圈数不保证（需要 homing，属 P2）。
2. **游标多重圈的台架验证**：主机单测只能覆盖圈内累积与纯函数 unwrap；
   `reducer=20` 时每转子圈出轴 0.314 rad，跳变阈值 0.5 rad 是安全边界，
   真实硬件（reducer=50）每圈 0.126 rad，余量更大。**首次上台架必须先用
   `motion pos_multi` 走两圈并观察 `motion show` 的 `travel` 单调性。**
3. **CAN owner 仲裁未实现**：CAN 与 USB 终端可互相覆盖 setpoint（同模式）；
   冲突仅靠 `MODE_LOCKED` 等既有保护。owner 仲裁仍是待办项。
4. **MIT 的 Kd 仅靠 observer**：`kd_max=2.0` 是保守占位值，需实测
   `motion show` 的速度纹波后确认或下调。

6. **多圈 P 项必须用连续误差（已修，2026-10-27 台架）**：`algo_motion_position()`
   内含 `foc_wrap_pm_pi`（单圈最短路径），多圈模式误用它会把整圈误差折成 0
   → 表现为"走到与目标差整圈的位置就停住"（实测：`pos_multi 400` 停在 40°、
   `pos_multi 720` 停在 0°）。修复：新增 `algo_motion_position_cont()`
   （误差不 wrap），`app_motion_position_step()` 按 `s_position_multi` 选择。
   回归：`test_algo_position_p`（差 2π/3π 的两种语义对比）。
