# 保护动作层设计（v2：分级动作 + 独立停机通道）

> 状态：设计待 Kaiser 裁决（2026-10-27，第 2 稿——已并入 builder check 的 9 条缺口）
> 前置：`docs/superpowers/specs/2026-09-19-fault-protection-design.md`（v1 检测层，已实现并台架验证）
> 关联：`2026-10-26-vbus-sampling-sync`（30kHz vbus 快通道，本设计 L3 的数据源）
> 红线：Kaiser 裁决「fault 保留快环，由 ADC1 中断回调负载」不变；本设计**不移动 fault tick**。

## 1. 目标与范围

**目标**：把 v1 的"检测 + 故障码 + 状态机"接到动作层，使保护在**不依赖单一失效点**的前提下切断能量，并保留现场可诊断性。

**做**：

1. **逐故障动作策略表**取代 `shutdown_en` 全局开关（可区分"告警/降额/受控停机/立即关桥"）；
2. **动作语义四级**（`WARN/DERATE/STOP/TRIP`），STOP 复用既有软停机状态机以防反灌；
3. **独立停机通道叠层**：补上"控制链已死"（ISR 心跳）与"CPU 已挂死"（EWDG）两条今天不存在的路径；
4. **L3 母线升级到 30kHz 快通道 + 两级阈值**（检测延迟 1ms → 33µs）；
5. **L1 WDOG 自动重臂**（消除"单次瞬态长期解除该相硬件保护"）；
6. **修正 L2 数据源缺陷**（见 §2.4——这是本轮发现的既有缺陷，不是新功能）；
7. **可观测 + 台架注入**（`fault policy` / `fault inject`），使每条动作路径可隔离验证。

**不做（明确排期外）**：

- ibus / DC 功率限 / 制动电阻（**硬件无采样**）；
- 过温折返（NTC 型号仍是占位，须先定选型；通路无需改动）；
- 失步/Spinout 检测、无感后备（可靠性专项，另立项）；
- **非故障码类**的四条动作门（见 §4.2 清单）——本设计只做"故障码 → 动作"，那四条列为开放项；
- 不改 ADC0 PMT 队列形态、ADC1 SEQ 长度、ISR 抢占策略。

## 2. 现状精确盘点（2026-10-27 逐条读码核实）

### 2.1 检测层

| 级别 | 判据 | 位置 | 实测去抖/窗口 |
|---|---|---|---|
| L1 | ADC 硬件 WDOG（三相 raw 越界 ≈±72.9A） | `app_fault_on_wdog()` ← ADC0 ISR | 无（硬件立即） |
| L2 | RMS > `oc_trip_a` | `app_fault_tick()` @1kHz | 连续 5 次，**窗口 250ms（见 §2.4）** |
| L3 | `V_VBUS` >36V / <9V（**1kHz 缓存值**） | 同上 | 连续 5 次 = 5ms |
| 健康 | PMT 帧序号停滞 >10ms | 同上 | 无 |
| 健康 | 编码器错误计数增量 ≥3 | 同上 | 无 |

另有电流环内置快跳 `app_foc_current_is_tripped()`（`i_trip_a`，峰值口径，连续 2 拍 →
零矢量+关桥；当前 `i_trip_a = 0.0` **关闭**，且**不产生故障码**）。

### 2.2 动作机制（已写好，被两个总开关停用）

`APP_PROTECT_ACTION_EN = 0`（`app_protect_policy.h`）+ `fault.shutdown_en: false`（YAML）。

| 路径 | 位置 | 语义 |
|---|---|---|
| FOC ISR 每拍门控 | `app_foc_isr_step()`:446 | `app_foc_current_protect()` + `app_3phase_inverter_emergency_stop()`（force_low + 关 +12V），锁存 `s_isr_inhibited`。**未受 `APP_PROTECT_ACTION_EN` 门控** |
| 主循环状态机 | `app_foc_run_body()`:292 | `zero_vector` + `emergency_stop` + `s_state = FAULT`（受门控） |
| 快跳过流 | `app_foc_run_body()`:301 | 同上（受门控） |
| 使能拒绝 | `app_foc_enable()`:769 | 故障非 NORMAL 时拒绝（受门控） |
| 清除 | `app_fault_clear()` | 条件复检 + 临界区 + WDOG 重臂；条件未恢复则拒绝 |

### 2.3 缺口清单

| # | 缺口 | 后果 |
|---|---|---|
| G1 | 动作策略是单一布尔 | 无法"OV 停机而 ENC_READ 只告警" |
| G2 | 停机语义只有 `emergency_stop`(force_low) | 高速 force_low → 失控减速 → 反灌抬压 |
| G3 | 可停机通道只有 FOC ISR | ISR 停摆 → PWM 保持最后占空比**永久输出** |
| G4 | 全工程未使用 EWDG/PEWDG | CPU 挂死 = 桥保持带电 |
| G5 | L3 用 1kHz 缓存 vbus | 检测延迟 1ms；回馈抬压被欠采样 |
| G6 | L1 WDOG 一次性（仅 `fault clear` 重臂） | 单次瞬态后该相硬件保护长期失效 |
| G7 | `app_fault_tick` 寄生 ADC1 SEQ ISR | ADC1/PWM 触发失效 → L2/L3/健康全停摆 |
| G8 | 无 ibus | 无 DC 功率限（硬件限制，已接受） |
| G9 | I²t 预算耗尽只静默回落 | 持续大电流无告警路径 |
| G10 | `fault` 看不到 policy/去抖进度/重臂次数/DERATE 值 | 台架定位靠猜 |
| **G11** | **L2 的 RMS 窗口与采样率与注释/spec 不符（见 §2.4）** | **L2 的实际响应是 ~250ms 且被混叠污染，"过流保护"名不符实** |

### 2.4 G11 详述（本轮新发现的既有缺陷）

`app_fault_process()` 的**唯一调用点**是 `app_logic.c:337`，位于 **1kHz 的 `app_io_step`**。
而 `app_fault.h:94` 注释写"25kHz 主循环"、`app_fault.c:72` 写"10ms 滑窗 @25kHz"、
v1 spec §5 写"25kHz 主循环"——**三处注释均与实现不符**。

实际后果：

1. `APP_FAULT_RMS_WINDOW_DEFAULT = 250` 在 1kHz 下 = **250 ms 窗口**（不是 10 ms）；
2. 输入是**以 1kHz 从固定 PWM 相位锁存**的三相电流 → 对 30kHz 开关纹波与最高
   550Hz 电频率严重欠采样（混叠）；
3. 因此 L2 的语义是"**~250ms 的准直流有效值监测**"，**不是过流保护**。
   持续过流的最短检出时间 ≈ 255ms（窗口填满 + 5ms 去抖）。
4. 真正的过流保护只剩 L1 硬件 WDOG（µs 级）与 `i_trip_a`（当前关闭）。

**设计含义**：L2 的动作类应是 **DERATE**（或最宽限的 STOP），而非快速 STOP；
且"L2 数据源修正"（30kHz 喂 `update_sq`，窗口重标为 10ms）应作为 L2 正确性的前置——
但它会改变今天已台架验证的行为与阈值，故列为**待裁决项 E**。

## 3. 约束

1. 硬件无 ibus → 能量判据只能用 `v_bus`/`|iq|`/`ωe`/`duty` 派生。
2. 板级无硬件 FAULT 输入 → MCU 是唯一可关桥主体。
3. fault tick 保留在 ADC1 ISR，分频 `pwm_freq/1000`，节拍/去抖语义不变。
4. 非抢占 ISR：动作路径必须短、无 flash 间接调用。
5. 1kHz 任务单次 ≤200µs；长操作走 job 框架。
6. **存在长时间的合法关中断/阻塞窗口**：flash 擦写（`rom_xpi_nor_program/erase` 期间 MIE=0）、
   `foc off` 的 `app_foc_wait_off(3000)` 阻塞（在 io 任务内自旋，FOC ISR 仍在跑）。
   → 任何"时基类"判据必须显式处理这些窗口（§4.3、§4.5）。
7. 标定/vtest 路径不豁免；动作开启后需重跑五个标定器回归。
8. 台架纪律不变（外部供电；`debug` 层可一键回 WARN-only）。

## 4. 设计

### 4.1 动作语义（四级）

```c
typedef enum {
    APP_FLT_ACT_WARN   = 0,  /* 只计数/告警（= 今天全部行为） */
    APP_FLT_ACT_DERATE = 1,  /* 按倍率压低 iq 上限；条件恢复自动解除 */
    APP_FLT_ACT_STOP   = 2,  /* 受控停机：卸转矩 → 缓降 → 软关断；无效状态降级 TRIP */
    APP_FLT_ACT_TRIP   = 3,  /* 立即关桥：force_low + 关 +12V + 锁存抑制 */
} app_fault_action_t;
```

- **DERATE 不关桥**：`app_fault_get_limit_scale()` 返回 `[0,1]`，由
  `app_foc_current_get_i_limit()` 乘入 → 经既有路径自动传播到 FOC 与 motion 转矩上限。
- **STOP 复用既有软停机**：`app_foc_request_stop()`（100 拍 1ms 缓降 + 20 拍零给定 +
  等待轴端速度 ≤0.5 rad/s 或最多 2s + `app_3phase_inverter_disable_soft()`，
  **先关 +12V 再停 PWM**）。保护侧只加"来源标记 + 超时兜底"。
- **TRIP 用既有路径**：`app_foc_isr_emergency()` / `app_3phase_inverter_emergency_stop()`（幂等）。

### 4.2 逐故障动作策略表（**policy 表本身就是开关**）

`policy[bit]`：init 期从 YAML 加载，运行期可改（RAM）。

**关键定案**：**不再新增总开关**。`warn` 即等于关闭；把某码设为 `trip` 就精确地只开那一条动作。
`APP_PROTECT_ACTION_EN` 的语义收窄为"**台架总保险**：置 0 则强制全部按 WARN 处理"
（保持既有"一键回台架态"能力，回退路径不变）。

| bit | 故障码 | 默认 | 理由 |
|---|---|---|---|
| 0-2 | `OC_FAST_U/V/W` | **TRIP** | 硬件级瞬态，不该由控制链仲裁；路径已存在 |
| 3-5 | `OC_SLOW_U/V/W` | **DERATE** | 实际是 ~250ms 准直流有效值（§2.4）→ 过载/发热语义，不是快速过流 |
| 6 | `VBUS_OV` | **STOP**（+ 二级 TRIP，§4.4） | 卸转矩本身就是降压手段，优于断桥 |
| 7 | `VBUS_UV` | **STOP** | 欠压继续拉电流会崩母线 |
| 8 | `ADC_TIMEOUT` | **TRIP** | ISR 已死 = 控制链不可信，且 PWM 会保持旧占空比 |
| 9 | `ENC_READ` | **WARN** | 事件型、误报风险高；先观察再定 |
| 10 | `OC_TRIP`（**新增**，见 §4.7） | **TRIP** | 电流环自身快跳，硬编码动作，但**产生故障码**以便可诊断 |

**锁存与使能契约（定案）**：

- 所有码在 `fault_raise` 时锁存（同今天）；
- **`app_foc_enable()` 只在 `latched` 含"带动作码"（policy ∈ {STOP, TRIP}）时拒绝**；
  纯 WARN/DERATE 的锁存**不阻塞** `foc on`（消除"nuisance ENC_READ 挡住使能"的问题）；
- STOP 完成后该码同样**锁存并要求 `fault clear`**（安全优先）；条件一恢复，`active` 自动解除，
  此时 `fault clear` 立即成功。

**明确排除在本次范围外**（`APP_PROTECT_ACTION_EN` 当前门控的**非故障码**路径，
builder check 指出它们会被"翻总开关"顺带打开——所以本次**不翻总开关**）：

| 路径 | 位置 | 处置 |
|---|---|---|
| 编码器降级 streak ≥32 → force_low | `app_foc.c:495-518` | 开放项：应与 `ENC_READ` 统一为单一码（`ENC_LOST`），单独立项 |
| ISR 超预算 → `s_isr_disabled=true` + emergency（仅 `foc off` 清除） | `app_foc.c:675-678` | 开放项：需独立码 + 可恢复语义 |
| `is_tripped` 锁存（仅 `foc off/on` 清除） | `app_foc_current.c:716-737` | 本设计给码（bit10），锁存清除仍由 `foc off`（§4.7） |
| `app_foc_enable()` 对"任意非 NORMAL"拒绝 | `app_foc.c:769` | 本设计改为"仅带动作码拒绝"（见上） |

### 4.3 独立停机通道叠层（核心）

| 通道 | 主体 | 触发源 | 时延 | 覆盖 |
|---|---|---|---|---|
| **A** | FOC ISR（30kHz 门控） | `app_fault_get_state()==FAULT` / estop 邮箱 / `is_tripped()` | ≈33µs | 故障已知且控制链存活 |
| **B** | 1kHz io 任务（`app_foc_run_body`） | 同 A（状态机兜底） | ≤1ms | ISR 漏判/状态未同步 |
| **C（新）** | **ISR 心跳检查**（1kHz io 任务） | `intf_clock_get_cycle()` 与"ADC0 PMT 钩子最后进入时刻"之差 > `isr_death_ms` | ≤10ms | **PMT/ADC 链死 → PWM 保持旧占空比**（G3/G7） |
| **D（新）** | **EWDG**（独立时钟源，`ewdg_init_interrupt_reset`） | **由 ADC0 PMT ISR 刷新**；超时 `ewdg_timeout_ms` → **复位** | ≤`ewdg_timeout_ms` | **CPU 挂死 / 调度器死 / 中断全关死循环**（G4） |

**通道 C 的时基与窗口处理（定案）**：

- 时间戳由 `app_fast_step_hook()`（ADC0 PMT 钩子）每拍写入一个 `.noncacheable` 全局；
- 检查点放在 `app_io_step` **顶部**；
- **必须排除合法的关中断/阻塞窗口**：`flash_enter_critical()` 区间、
  `app_foc_wait_off()` 自旋、`app_3phase_inverter_enable()` 的 10ms 等待。
  实现：这些区间前后显式"重基线"（写一个 `baseline_cycle = now`），
  并在 flash job 活跃期间抑制判定。**否则 `param save` / `cal cogging save` / `foc off` 会误触 TRIP。**
- 判据用"ISR 未推进"而非"任务未推进"，因此 `foc off` 的 3s 阻塞**不**影响（ISR 仍在跑）。

**通道 D 的喂狗点与超时（定案）**：

- **由 ADC0 PMT ISR 刷新**（不在 io 任务——io 任务有 3s 级合法阻塞，那里喂狗毫无意义）；
- 于是"超时"同时覆盖：CPU 挂死、调度器死、**ISR 停摆**（此时通道 C 已先做优雅关桥，D 是最后兜底）；
- `ewdg_timeout_ms` **必须大于最长合法 MIE=0 窗口**（flash 擦写）。该窗口**尚未实测** →
  阶段 3 第一件事是加一个"最长关中断窗口"计数器（`flash_critical_max_us`，与 `isr_cycles_max` 同风格）
  并台架实测，再定 `ewdg_timeout_ms`（暂定 100ms，实测后确认）。

**通道 D 的复位安全性（已核实）**：

- `PIN_GDRV_12V_EN` = `pull=PULL_DOWN` + `init_level=LOW` → **复位后栅极供电天然关闭**；
- PWM 通道未使能 → 输出不导通；IOC 复位后 PA20-25 回默认功能；
- `intf_sys_get_reset_status()` 已解码 bit16/17（看门狗）、bit24（PMIC 看门狗）→ `fault` 可报告复位源。
- **须台架确认**：EWDG 复位后 +12V 轨与三相输出的实际电平（示波器看一次）。

### 4.4 L3 母线：30kHz 快通道 + 两级阈值 + **单一数据源**

数据源：`app_analog_signal_vbus_fast()`（与三相电流**同源触发、同拍**；无效时回退 1kHz 缓存并计 `stale`）。

**定案：单一数据源 + ISR 内计数，不保留 1kHz 副本**（避免两个"1ms"定义不同的计数器互相矛盾）：

- 比较与去抖计数放在 **FOC ISR 内**（每拍已有 `v_bus`），且**位置前移到 OFF/FAULT 早退之前**
  （今天 `v_bus` 的读取在 `app_foc.c:558`，位于 `:456` 的 OFF/FAULT 早退之后 → ISR 侧检测
  在 FOC 关闭时不会执行。**必须前移**，否则"桥使能但 FOC OFF"这一最坏反灌场景无 OV 保护）。
- 该段必须 `FOC_ATTR_RAMFUNC`（ILM），只做"比较 + 计数 + 置位"；
- `app_fault_tick` 只保留 L2（RMS）与健康项，**不再做 L3 判断**。

| 级 | 阈值（YAML） | 默认 | 动作 | 去抖 |
|---|---|---|---|---|
| 一级 | `fault.vbus_ov_v` | 36.0V | **STOP** | `vbus_ov_ms` 默认 1ms（= pwm_freq/1000 拍） |
| 二级 | `fault.vbus_ov_trip_v` | 40.0V | **TRIP** | 无（单拍，硬件风险优先） |
| 欠压 | `fault.vbus_uv_v` | 9.0V | **STOP** | `vbus_uv_ms` 默认 1ms |

### 4.5 L1 WDOG 自动重臂（消除 G6）

- 命中 → `fault_raise()` → 记录 → 启动 `wdog_rearm_ms`（默认 100ms）；
- 到期且**三相 raw 均回到窗口内** 连续 N 拍 → `app_adc_wdog_reenable()`；
- `wdog_rearm_max`（默认 5）：超限 → 该相升 **TRIP 锁存**（不再自动重臂，仅 `fault clear` 复位）；
- 重臂服务由 `app_fault_tick`（1kHz）推进——它自身依赖 ADC1 SEQ 链，
  但 WDOG 是硬件级，且 io 任务侧另有通道 C/D 兜底。

### 4.6 无 ibus 条件下的 DERATE 来源（**范围收窄**）

| 来源 | 数据源 | 释放条件 | 本轮落地？ |
|---|---|---|---|
| I²t 预算耗尽 | 既有 `s_i_limit_credit` | credit 回充到 `i_q_boost_recover_ratio`（0.9，复用既有滞环） | **是** |
| 台架注入 | `fault inject` | 手动 off | **是**（用于验证通路） |
| 过温 | NTC0/NTC1（已采到） | 需选型定案 + 折返曲线 | 否（排期外） |
| 功率代理 `\|iq\|·\|ωe\|/pp` | 推算 | — | **否**：无 ibus 时这是电功率近似，包络值必须实测标定；**无可信数值 → 不落地**（避免写一个拍脑袋的阈值） |
| 栅极状态不一致 | `gate_power_is_on()` vs `is_enabled()` | — | 是，但动作是 **TRIP**（不是 DERATE） |

DERATE 倍率固定 `derate_scale`（默认 0.5）；变化不加速率限制，
依赖既有 I²t 滞环（0.9 比例）避免 1kHz 带振荡。

### 4.7 快跳过流产生故障码（G10 的一部分）

新增 `APP_FAULT_OC_TRIP (1UL << 10)`：`app_foc` 在检测到 `app_foc_current_is_tripped()` 时
同时 `app_fault_raise(APP_FAULT_OC_TRIP, 0)`，使它能出现在 `fault show` 与首故障快照里。

- 动作：**硬编码 TRIP**（电流环自身的保护，不查表）；
- 恢复：`is_tripped` 锁存仍由 `foc off` 清除（既有行为）；码位由 `fault clear` 清除。
  → **两步恢复**，`fault show` 明确提示 `recovery: foc off required`。

### 4.8 可观测与台架注入

- `fault` 命令新增列：每码 policy 类、去抖进度/阈值拍数、`wdog_rearm_cnt`、当前 `limit_scale`、
  当前最高级动作、复位源解码（`rst_status`）、快跳恢复提示。
- **`fault inject <code|name> on|off`（新）**：软件注入（RAM），绕过物理阈值。
  价值：每条动作路径可隔离验证，不需改阈值（v1 spec §9 用改阈值法，风险更高）。
- `fault policy show` / `fault policy <code> warn|derate|stop|trip`（运行期 RAM）。
- Ozone 新增：`g_fault_action`、`g_fault_limit_scale`（`.noncacheable`）。

## 5. 接口（新增/变更精确签名）

```c
/* App/Control/Inc/app_fault.h */
#define APP_FAULT_OC_TRIP (1UL << 10)   /* 新增：电流环快跳 */

typedef enum {
    APP_FLT_ACT_WARN = 0, APP_FLT_ACT_DERATE = 1, APP_FLT_ACT_STOP = 2, APP_FLT_ACT_TRIP = 3,
} app_fault_action_t;

void               app_fault_set_policy(uint32_t code, app_fault_action_t act); /* 运行期 RAM */
app_fault_action_t app_fault_get_policy(uint32_t code);
app_fault_action_t app_fault_get_action(void);         /* 当前生效的最高级动作 */
float              app_fault_get_limit_scale(void);    /* [0,1]；无 DERATE = 1.0；ISR 可读 */
int                app_fault_inject(uint32_t code, bool on);   /* 台架注入（RAM） */

/* ISR 侧接线（均在 ILM） */
void app_fault_vbus_step(float v_bus_v);   /* FOC ISR 每拍：L3 比较/去抖（OFF 态亦调用） */
void app_fault_isr_pulse(void);            /* FOC ISR 每拍：刷新心跳时间戳 */

/* 任务侧（1kHz io 任务） */
void app_fault_heartbeat_check(uint32_t now_cycles);  /* 通道 C：ISR 停摆 → OC/ADC_TIMEOUT */
void app_fault_critical_window(uint32_t now_cycles);  /* 合法关中断窗口后重基线 */
void app_fault_wdog_service(void);                    /* L1 自动重臂（也可由 tick 推进） */
```

```c
/* App/Control/Inc/app_foc.h */
int  app_foc_request_fault_stop(uint32_t code);  /* STOP 入口；CALIB/vtest/OFF-桥使能时返回 -1 */
bool app_foc_fault_stop_pending(void);
```

```c
/* Interface/Inc/intf_ewdg.h + Driver/hpm_impl/drv_ewdg.c */
int  intf_ewdg_init(uint32_t timeout_ms, bool reset_on_timeout); /* 0=成功 */
void intf_ewdg_refresh(void);
```

**分层**：`app_fault`（Control）只依赖 Interface 与 Platform；EWDG 契约在 `Interface/`，
实现在 `Driver/hpm_impl/`；喂狗策略在 `App/Platform/Src/app_watchdog.c`（或直接放 app_fault 的
`isr_pulse` 内调用 `intf_ewdg_refresh()`）。符合 AGENTS §2。

## 6. 参数（`config/software.yaml` 新增）

```yaml
fault:
  shutdown_en: false          # 【阶段 5 退役】语义被 policy 表覆盖
  action:                     # 逐故障动作（warn|derate|stop|trip）
    oc_fast: trip
    oc_slow: derate           # 见 §2.4：L2 是 ~250ms 准直流监测
    vbus_ov: stop
    vbus_uv: stop
    adc_timeout: trip
    enc_read: warn
    oc_trip: trip             # 硬编码，此处仅文档化
  vbus_ov_trip_v: 40.0        # 二级硬跳 [V]
  vbus_ov_ms: 1               # 一级去抖 [ms]（快通道，按 pwm_freq 折算）
  vbus_uv_ms: 1
  isr_death_ms: 10            # ISR 心跳超时 [ms]
  ewdg_timeout_ms: 100        # EWDG 复位超时 [ms]；0=关闭；须 > 最长 MIE=0 窗口
  derate_scale: 0.5           # DERATE 生效的 iq 倍率
  wdog_rearm_ms: 100          # WDOG 自动重臂延时 [ms]
  wdog_rearm_max: 5           # 单次上电重臂上限（超限升 TRIP）
```

均为 `APPLY_REBOOT`（init 期消费），改后需 `make artifacts` + 重新烧录；
运行期经 `fault policy` 临时覆盖（RAM，不持久化）。

## 7. 实施步骤（阶段 0~5）

> 原则：每阶段可单独停机回退；阶段 0 **零行为变化**。

### 阶段 0：装开关但不合闸（纯可观测）

- 改动：
  - `app_fault.{h,c}`：`app_fault_action_t`、`policy[]`、`app_fault_get_action()`、
    `app_fault_get_limit_scale()`、`app_fault_inject()`、`APP_FAULT_OC_TRIP` 码；
    `fault_raise` 按 policy 分级（WARN 路径与今天同形）；`app_foc_enable` 改为"仅带动作码拒绝"。
  - 终端：`fault policy show|set`、`fault inject <code> on|off`、`fault` 增列。
  - `config/software.yaml` + `scripts/gen_params.py`：`fault.action.*`。
  - 主机回归：新增 `scripts/tests/fault/`（policy 查表、取最高级、inject 语义、limit_scale、
    "仅带动作码拒绝使能"）。
- 出口判据：全量回归绿；**所有 policy 默认 WARN** → `fault show` 与今天逐字段一致（新增列除外）；
  `fault inject enc_read on` → `codes` 置位但 `state=WARNING`、桥 duty 逐位不变（示波器确认）；
  `fault inject enc_read on` 后 `foc on` **仍被拒绝**（因锁存）→ 记录该行为并确认这是期望语义。

### 阶段 1：两条 TRIP 隔离上线

- 目标：只开 `oc_fast = trip` 与 `oc_trip`（同时把 `i_trip_a` 从 0 改为非 0，**需裁决 D**）。
- 改动：**无新代码**（路径已存在），仅 YAML。**不翻 `APP_PROTECT_ACTION_EN`**。
- 台架：`fault inject oc_fast_u on` → ≤1 拍 force_low + 关 +12V + 锁存；`fault clear` 后可重新使能；
  `fault inject oc_trip on` → 同前，且 `fault show` 出现 `code=0x400`，恢复需 `foc off`。
- 出口判据：注入/恢复闭环成立；静止与 `motion speed 5/20` 各一次，USB CDC 不断连
  （若断连 → 走 AGENTS「PWM 正常关断瞬态」排查路径，不改此处）。

### 阶段 2：L3 快通道 + STOP 受控停机

- 目标：L3 单一数据源（ISR 内、前移到早退之前）+ 两级阈值；OV 一级 STOP / 二级 TRIP；UV STOP。
- 改动：`app_fault_vbus_step()`；`app_foc_request_fault_stop()`；`app_fault_tick` 移除 L3；
  `app_foc_run_body` 按 policy 分派 STOP/TRIP；**STOP 在 {OFF-但桥使能, CALIB, vtest} 下降级 TRIP**。
- **需裁决 B**：STOP 的等待上限（沿用既有 ~3s，还是给故障来源更短硬上限）。
- 台架：外部电源缓升过 36V → STOP（转矩归零→缓降→软关断，**不是** force_low）；过 40V → TRIP；
  在"桥使能 + FOC OFF"（`driver power on` + `foc off`）状态下升压 → 必须能触发（验证前移有效）。
- 出口判据：`cal rl` / `cal cogging` 全程不误触；降速段 `pvb` 计数相比今天不新增。

### 阶段 3：独立停机通道（G3/G4）

- 改动：`Interface/intf_ewdg.h`、`Driver/hpm_impl/drv_ewdg.c`、喂狗接线（`app_fault_isr_pulse`）；
  `app_fault_heartbeat_check()` + `app_fault_critical_window()`（`app_io_step` 顶部）；
  `app_logic.c` 初始化顺序；`fault` 报告复位源；
  **新增 `flash_critical_max_us` 计数器并实测最长 MIE=0 窗口**，据此定 `ewdg_timeout_ms`。
- **需裁决 A**：是否接受"EWDG 复位即停机"（唯一覆盖 CPU 挂死的通道；会丢现场，
  `rst_status` + `g_rtos_exc_*` 指纹仍在）。以及走"先中断后复位"还是"直接复位"。
- 台架：① 临时停 PWM 触发 → ≤10ms 关桥（通道 C）；② 临时 `while(1)` → ≤100ms 复位，
  复位源可读，复位后 +12V 关（示波器）；③ `param save` / `cal cogging save` / `foc off` 各 10 次
  → **不发生** EWDG 复位与通道 C 误触（验证 §3.6 的窗口处理）。
- 出口判据：三条通道各自可独立触发与观测；正常运行 ≥30min 无 EWDG 复位。

### 阶段 4：DERATE（范围收窄版）

- 落地来源只有两个：I²t 预算耗尽 + `fault inject`；过温与功率代理明确不做（§4.6）。
- 台架：`fault inject oc_slow_u on` → `app_foc_current_get_i_limit()` 与 motion 转矩上限同步下降；
  off 后回满；确认无 1kHz 带振荡。
- 出口判据：`foc` 的 `sat`/`util` 行为合理；I²t 真实耗尽路径（持续 boost 2s）同样触发 DERATE。

### 阶段 5：退役旧开关 + 文档收尾

- `shutdown_en` 退役；`APP_PROTECT_ACTION_EN` 语义固定为"台架强制 WARN"；
  v1 spec §10 的"动作层（v2）"条目结案；AGENTS.md 增补本节约定与 §2.4 的 L2 事实更正。

## 8. 台架验证总表

| 项 | 方法 | 通过判据 |
|---|---|---|
| policy 查表 | 主机用例 + `fault policy show` | 与 YAML 一致 |
| WARN 无副作用 | 逐码 `fault inject` | duty/电流/状态机逐位不变 |
| 使能契约 | 注入 WARN 码后 `foc on` | 带动作码拒绝；WARN/DERATE 不拒绝 |
| TRIP 闭环 | inject `oc_fast_*`、`adc_timeout`、`oc_trip` | ≤1 拍 force_low + 关 12V + 锁存；可恢复 |
| STOP 闭环 | 升压过 36V | 转矩归零→缓降→软关断；**非** force_low |
| OV 二级 | 升压过 40V | 立即 force_low |
| OV @FOC OFF | `driver power on` + `foc off` 下升压 | 能触发（验证 §4.4 前移有效） |
| 检测时延 | 快通道 OV + 示波器 | ≤50µs（对比 1kHz 基准 1ms） |
| ISR 心跳 | 停 PWM 触发 | ≤10ms 关桥 |
| 心跳不误触 | `param save`/`cal cogging save`/`foc off` ×10 | 无误触 |
| EWDG | 临时 `while(1)` | ≤100ms 复位；复位源可读；复位后 12V 关 |
| WDOG 重臂 | 注入单次瞬态 | 100ms 后自动重臂；≥5 次升 TRIP |
| DERATE | inject I²t | `i_limit` 下降且可恢复；无振荡 |
| 全链路回归 | 五个标定器 + `motion speed 5/20/30` | 不误触；USB CDC 不断连 |

## 9. 风险与回退

| 风险 | 缓解 | 回退 |
|---|---|---|
| 动作开启打断台架调试习惯 | 阶段 0 全 WARN；`APP_PROTECT_ACTION_EN=0` 一键回台架态 | 置 0 即恢复今天行为 |
| STOP 总时延 ~3s 不符保护预期 | `fault_stop_speed_wait_ms`（裁决 B） | policy 改 `trip` |
| EWDG 复位丢现场 | `rst_status` + `g_rtos_exc_*` | `ewdg_timeout_ms=0` |
| EWDG 在 flash 擦写窗口误复位 | 先实测 MIE=0 窗口再定超时 | 同上 |
| 通道 C 误触（合法阻塞） | 显式重基线 + flash job 抑制 | `isr_death_ms=0` |
| WDOG 重臂反复触发 | 重臂上限 + 电流回落条件 | `wdog_rearm_max=0`（退回一次性） |
| 快通道 OV 受开关纹波误触 | 一级去抖 1ms；36V 对 24V 余量 50% | 增大 `vbus_ov_ms` |
| L2 语义被误当快速过流 | 默认改 DERATE + 文档纠正（§2.4） | policy 改 `stop` |
| 分层违规 | EWDG 走 Interface/Driver 契约 | — |

## 10. 需要 Kaiser 裁决的项

Mephisto 已自行定案的（可复核）：policy 即开关、不翻 `APP_PROTECT_ACTION_EN`、
`app_foc_enable` 仅带动作码拒绝、STOP 在所有锁存、L3 单一数据源 + ISR 前移、
EWDG 由 ISR 喂狗、DERATE 范围收窄（功率代理不落地）、`OC_TRIP` 新码与两步恢复。

待裁决：

| # | 事项 | Mephisto 建议 |
|---|---|---|
| A | **EWDG 复位即停机是否接受**（阶段 3 前置） | 接受。这是唯一覆盖 CPU 挂死的通道；`rst_status`/`g_rtos_exc_*` 保现场 |
| B | **STOP 的等待上限** | 沿用既有 ~3s（最大防反灌）；若要求更快，加 `fault_stop_speed_wait_ms=500` |
| C | **TRIP/STOP 分类**（尤其 `VBUS_OV = STOP`） | 同意 STOP：卸转矩本身降压，比断桥更有效；40V 二级兜底 |
| D | **`i_trip_a` 取值**（阶段 1 要打开，现为 0） | `1.5 × i_peak_10s ≈ 36.5A`（峰值口径，连续 2 拍） |
| E | **L2 数据源是否本轮修正**（1kHz/250ms → 30kHz/10ms） | 建议**单独立项**：它改变已台架验证的阈值语义，不宜混在动作层里；本轮先把 L2 默认动作定为 DERATE |
| F | **阶段顺序** | 先只做阶段 0（零行为变化），验证 OK 后逐段放开 |