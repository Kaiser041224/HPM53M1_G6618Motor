# FOC 电流环 × FreeRTOS 框架合并设计（feat/foc-v1 ← feat/rtos-foundation）

| 项 | 值 |
| :--- | :--- |
| 日期 | 2026-09-23 |
| 状态 | 已批准（设计两节经 Kaiser 逐节确认） |
| 目标分支 | `feat/foc-v1`（合并方向：`feat/rtos-foundation` → `feat/foc-v1`） |
| 分叉点 | `a8c5d2c`（foc-v1 领先 66 提交 / rtos-foundation 领先 14 提交） |
| 里程碑 | M1：仅 FOC 电流环可测；辨识、V/F、速度环不在本期 |

---

## 1. 目标与范围

**目标**：把 rtos-foundation 的 FreeRTOS 框架并入 foc-v1 的代码结构，产出「电流环最小可测」镜像，跑通
**开环 vtest → 闭环 iq 阶跃**（offset=0，不辨识）。

**范围外（本期不做）**：
- 电机辨识（`app_motor_identify`、`id_encoder`、`cal encoder`）——编译级裁掉
- V/F 开环旋转（`app_debug_motor`、`motor start/stop/freq/mod`）——编译级裁掉
- 出轴编码器采样与游标比值（1kHz `sample_output`）——不接入
- 速度环、双编码器角度环、CAN 协议
- 任何保护的「停机动作」启用（见 §6）

---

## 2. 合并策略（方案 A：git 语义合并 + 最小 ISR 快车道）

```bash
git checkout feat/foc-v1
git merge --no-ff feat/rtos-foundation
# 9 个双改文件按 §3 裁决；app_logic.c / app_adc.* / CMakeLists.txt 按目标架构重写终态
```

保留双侧全部提交历史（rtos 侧 14 个提交为逐条上板验证记录，是排障资料）。

### 2.1 已定项

| 项 | 取值 | 依据 |
| :--- | :--- | :--- |
| 模块结构 | 取 foc-v1 侧（`App/Algorithm/FOC/`、`app_foc*`、`app_debug_foc`、`cmd_foc`） | 用户要求「代码结构参考 foc-v1」 |
| 编码器采样实现 | 取 foc-v1 侧（`app_encoder_read_rotor_isr` + `algo_encoder_snapshot` seqlock） | foc-v1 侧更新（坏帧/陈旧策略、有界重试） |
| ISR 挂接缝 | foc-v1 的 `app_adc_register_current_hook()`；rtos 的 `fast_cb` 计时语义并入 hook 包装 | 两者是同一缝（`adc_current_pmt_cb` 锁存后调用） |
| RTOS 框架件 | 取 rtos 侧：非向量模式 + 非抢占、`FreeRTOSConfig.h`、`app_rtos.*`、`app_rtos_tasks.*`、ISR 计时 | 见 §5；AGENTS.md「FreeRTOS 引入纪律」 |
| 占空比写路径 | 取 rtos 侧 `drv_hrpwm.c`（CMP 写无 UNLK） | 228kHz 触发风暴根治，勿回退 |
| 三角函数 | FOC 用 foc-v1 `foc_sincos`（256 表 + 插值）；`algo_trig` 随 V/F 一起不编译 | 语义等价，FOC 调用点已遍布 `foc_park*` |

---

## 3. 冲突面裁决（9 个双改文件）

| 文件 | 裁决 |
| :--- | :--- |
| `CMakeLists.txt` | **重写**：rtos 的 `set(CONFIG_FREERTOS 1)`（`find_package` 前）+ `sdk_compile_definitions(-DUSE_NONVECTOR_MODE=1)` + `(-DDISABLE_IRQ_PREEMPTIVE=1)`（`project()` 后）；源列表按 foc-v1 结构组织，剔除不编译项 |
| `App/Logic/app_logic.c` | **重写**：rtos 骨架（`app_fast_step_hook` / `app_io_step` / `app_diag_step`）+ foc-v1 内容（`app_foc_isr_step` 调用）。foc-v1 的 `app_run()` 裸机超循环废弃，内容拆入 fast/io/diag step |
| `App/Platform/Inc/app_adc.h` | **重写**：保留 foc-v1 的 `app_adc_register_current_hook()` 缝 |
| `App/Platform/Src/app_adc.c` | **重写**：同上；rtos 的 hook 计时语义并入 |
| `Driver/hpm_impl/drv_hrpwm.c` | **取 rtos 侧**（CMP 写无 UNLK） |
| `App/Debug/Inc/app_debug_encoder.h` | **取 foc-v1 侧**（seqlock）+ 并入 rtos 迟拍统计接口（`note_loop_late`） |
| `App/Debug/Src/app_debug_encoder.c` | **取 foc-v1 侧**；出轴采样入口同步移除（转子采样保留） |
| `App/Debug/Src/app_debug_motor.c` | **裁掉**（V/F） |
| `AGENTS.md` | 取 rtos 侧「FreeRTOS 引入纪律」；合并时逐条核对 foc-v1 侧新增的项目附加约定 |

---

## 4. 目标代码结构

```text
App/
├── main.c                    [rtos]     board_init → intf_clock_init → app_application_start
├── Logic/
│   ├── app_logic.c           [重写]     app_init / app_fast_step_hook / app_io_step / app_diag_step
│   └── app_rtos_tasks.c/.h   [rtos]     任务编排
├── Algorithm/
│   ├── FOC/Inc|Src/          [foc-v1]   foc_math / foc_current / foc_modulation / foc_angle
│   │                                     （id_encoder.c 不编译——辨识专用）
│   └── Inc|Src/              [foc-v1]   algo_pid/filter/ramp/rms/hyst/ffd/encoder_snapshot
│         algo_trig.*         [不编译]   V/F 专用
├── Control/
│   ├── app_foc.c/.h          [foc-v1]   状态机 + app_foc_isr_step；identify 调用点→空桩
│   ├── app_foc_current.c/.h  [foc-v1]   电流环 PI+调制+写桥
│   ├── app_fault.c/.h        [foc-v1]   保留编译；OV/UV 改告警（§6）
│   ├── app_motor_params.*    [共用]
│   └── app_motor_identify.*  [不编译]
├── Platform/
│   ├── app_rtos.c/.h         [rtos]     异常指纹 / fatal / tick 自检
│   ├── app_adc.*             [重写]     current_hook 挂接缝
│   ├── app_encoder.*         [foc-v1]   seqlock 快照 + read_rotor_isr
│   ├── app_3phase_inverter.* [foc-v1]
│   └── (analog/hrpwm/param…) [共用，foc-v1 侧为准 + rtos drv_hrpwm 例外]
├── Comm/terminal/
│   ├── app_terminal_cmd_foc.c    [foc-v1]  foc status|on|off|vtest|bench|trace
│   ├── app_terminal_cmd_motor.c  [重写]    仅 motor iq + cal current
│   └── (cmd_sys/diag/param…)     [共用]
└── Debug/
    ├── app_debug_foc.c/.h    [foc-v1]   Ozone 观测 g_app_debug_foc
    ├── app_debug_rtt.c/.h    [rtos]     rtt_log 任务 + app_debug_printf
    ├── app_debug_motor.c     [裁掉]
    └── (adc/encoder/…)       [foc-v1 侧为准 + rtos 迟拍统计并入]

config/    FreeRTOSConfig.h [rtos]；software/motor/hardware.yaml [foc-v1]
Driver/    [共用] + rtos: drv_adc ISR 计时 / drv_hrpwm 无 UNLK
Interface/ [共用]
```

不编译清单（CMake 剔除，源文件保留）：`app_motor_identify.c`、`id_encoder.c`、`app_debug_motor.c`、`algo_trig.c`。
`app_foc.c` 中 identify 的调用点（`app_motor_identify_fast_step` / `is_active` / `abort`）换空桩。

---

## 5. 运行架构

```text
main() → board_init → intf_clock_init → app_application_start()
  ├─ rtt_log 任务 (prio4)     日志队列 → SEGGER RTT
  ├─ io   任务 (prio3, 1ms)   首次: app_init + tick 自检
  │                           循环: app_analog_signal_process → app_fault_process
  │                                 → app_foc_run_once（状态机/给定编排）
  │                                 → app_debug_foc_tick → app_adc_slow_process
  │                                 → app_debug_adc_update → app_terminal_run_once
  └─ diag 任务 (prio3, 1s)    LED + 编码器统计 + [ISR] 统计

ADC0 PMT 中断 (25kHz, PLIC prio3, 非向量/非抢占)
  adc_generic_isr → adc_current_pmt_cb（锁存 i_u/i_v/i_w raw）
  → current_hook = app_fast_step_hook（mcycle 计时 min/avg/max）
      → app_foc_isr_step            [foc-v1 原样]
          → encoder_read_rotor_isr（seqlock 快照）→ foc_angle.step → θe,ωe
          → raw→A → app_foc_current_run_fresh
              → clarke → park → PI → inv_park → foc_modulation → set_duty_abc
```

**要点**
- 快车道内容就是 FOC 电流环；`app_fast_step_hook` 仅作 mcycle 计时包装直接调 `app_foc_isr_step`（foc-v1 的 ISR 边界）。
- `app_analog_signal_process` / `app_fault_process` 降入 1ms io 任务。VBUS 本就 ADC1 慢通道 1kHz 刷新，此举的时效损失为工程推断（≈同量级），需实测确认。
- `app_foc_run_once` 降 1ms（状态机/给定编排，非控制环）；iq 给定在阶跃测试时直通或斜坡 ≪ 1ms，避免 1ms 量化磨圆阶跃。
- ISR 硬约束（不变）：无 RTOS API / 无 printf / 无动态分配 / 无等待。
- **ISR 超预算停机保留**（`app_foc.c` `APP_FOC_ISR_BUDGET_US`）：防触发链风暴/系统静默，非电机保护，独立于保护 gate。

---

## 6. 保护策略：动作全关，检测保留

本阶段由**可调电源做外部保护**；所有保护的「停机/跳闸动作」不启用。检测与告警保留——fault 码记录「本应触发的保护」，为后续逐项加回提供实测阈值依据。

| 保护层 | 本期行为 | 实现 |
| :--- | :--- | :--- |
| 2 拍快速跳闸 `i_trip_a` | 关（yaml 默认 0） | 无需动作 |
| 转矩限速 `speed_max` | 关（yaml 默认 0） | 无需动作 |
| 故障停机 `shutdown_en` | **关**（`foc bench 1` 置 0） | 复用 foc-v1 台架开关 |
| 硬件 WDOG / L2 RMS 过流 / ADC 停滞 / 编码器错 | **仅告警不停机** | `shutdown_en=0` 既有语义 |
| 母线 OV/UV | **仅判断提醒，不动作** | **保护逻辑唯一改动**：`app_fault.c` OV/UV 分支由「无条件停机」改为「跟随 shutdown_en」（取消 `foc bench 1` 的 OV/UV 例外） |
| ISR 超预算停机 | **保留动作** | 防触发链静默（§5） |
| `app_rtos_fatal` / 异常指纹 | 保留 | 系统兜底 |

**OV/UV 改告警的依据**（Kaiser 2026-09-23 裁决）：本测试谱为静止 vtest / 静止 iq 阶跃，转速近零，反灌能量极小。限流电源不吸收反灌能量的事实不变——**旋转测试前必须把 OV 停机动作加回**。

**安全边界（真话）**：动作全关后，硬保护仅剩硬件死区 50ns（防直通）与 ISR 超预算停机。PSU 限流兜 ms 级电流失控；µs 级直通类故障由死区兜底，PSU 反应不及。

**操作纪律**：改占空比/参数前先 `foc off`；上电顺序 = PSU 限流先于电压。

---

## 7. 测试引导（M1 验收）

### 7.1 测试条件

- PSU：电压 **24V**（当前测试台母线；`vbus_nom=48V` 是电机额定，非台架值）；限流从 **2A** 起（覆盖 `vtest 0.10V` → 预期 0.63A），加大给定按**最大电流 × 2** 抬。
- iq 阶跃要干净波形 → 转子**机械夹持**；自由转子短窗口 trace（5ms）变形有限，可接受。
- 观测：USB Terminal（命令）+ RTT（`[ISR]` 行）+ Ozone（`g_foc_current_snapshot`、`g_app_debug_foc`）。

### 7.2 执行序列

```text
1  上电自检       RTT 心跳 + USB Terminal 可用；foc status → state=OFF
2  cal current    ADC 零点标定（256 帧 ≈ 10ms；FOC 必须 OFF）—— 电流环前置
3  foc bench 1    关自动停机保护；确认打印 i_trip=0 / speed_max=0 / shutdown_en=0
4  foc on         → READY 零矢量；foc status 复核 state=READY、无 fault 码
5  foc vtest 0.10  1.5s 自停；预期 |i| ≈ v/rs = 0.10/0.158 ≈ 0.63A（命令打印预期值）
6  foc vtest 0.2 90  换角度 → 电流矢量方向随之变化（角度通路活着）
7  foc trace → motor iq 1.0   5ms @25kHz 抓 dq 阶跃波形（trace 先 arm 再给定）
8  motor iq 2.0 / 0.5 / -1.0  回显 ref/avg/now 跟随；观察 v_scale 饱和标志
9  foc off        收工；记录 [ISR] 统计与 fault 码清单
```

### 7.3 验收判据

| 项 | 判据 | 依据 |
| :--- | :--- | :--- |
| vtest 幅值 | \|i\| ≈ v/rs，±20% 内 | 电阻限流稳态欧姆律 |
| vtest 符号/映射 | 正给定 → 正方向电流（`invert=1` 换算后） | 历史坑：电流采样符号反相（`9ba8ce3`） |
| iq 阶跃跟踪 | `now` → `ref`，无持续振荡；沉降 ≤ 2ms（500Hz 带宽设计，工程推断；开环 τ=Ls/Rs≈0.75ms 为被控对象参考量） | `software.yaml` current_loop |
| 电压饱和 | 稳态 `v_scale`=1.0（<1 = 饱和） | `app_foc_current` 快照 |
| ISR 预算 | `[ISR]` 单拍 **<40µs**，`headroom`>0 | M1 判据 |
| 无停机事件 | 全程无意外 emergency stop（告警码允许，逐条记录） | §6 |

### 7.4 失败排查（按概率）

1. **vtest 无电流** → `[ISR] n`=25000/s？（触发链）→ `foc status` state → 桥使能/占空比写路径（UNLK 教训：`drv_hrpwm` 必须 rtos 侧无 UNLK 版本）
2. **幅值偏差大** → `cal current` 是否做过；`a_per_volt=66.67` / `bias_v=1.65`；分流 2mΩ / 增益 7.5
3. **符号/相序错** → `hardware.yaml invert=1` 换算；U/V/W 相序对应
4. **iq 阶跃振荡** → kp=0.3 / ki=100（500Hz 设计值）；先查编码器坏帧与供电，再降带宽
5. **iq 反号或极弱** → offset=0 时 dq 帧与真实电角有恒定偏差：iq_measured→iq_ref 仍自洽可验，力矩角非最优属预期，非故障
6. **`[ISR]` 波动 / late 涨** → CMP10 触发扰动复发（`drv_hrpwm` UNLK 是否被回退）

---

## 8. 风险与回滚

| 风险 | 分级 | 缓解 |
| :--- | :--- | :--- |
| 占空比写路径 bug 致直通 | µs 级，PSU 不及 | 硬件死区 50ns；改 CMP 前 `foc off` |
| 保护动作全关后电流失控 | ms 级 | PSU 限流（测试谱 IQ ≤ 2A 起） |
| OV/UV 不停机 + 反灌抬压 | 旋转测试才成立 | **旋转测试前加回 OV 停机**（§6 明示） |
| 1ms 域 `fault_process` 时效下降 | 工程推断 | 实测确认；L2/L3 去抖按 1kHz 重标定 |
| 合并冲突解决引入回归 | — | 逐文件裁决（§3）；`make build` + 上板 M1 测试序列验证 |

回滚：合并提交为单个 `--no-ff` merge commit，`git revert -m 1 <merge>` 或 `git reset --hard <合并前>`。
不编译清单（§4）使裁剪可独立回滚（改 CMakeLists 一行即可复编）。

---

## 9. 事实与推断标记

- **事实**（来自分支代码/文档实读）：§2.1、§3、§4 结构、§5 调用链、命令语法与参数值（`rs=0.158`、`kp=0.3`、`ki=100`、`i_peak_10s=24.3`、`duty_max=0.885`、`a_per_volt=66.67`、PWM 25kHz、极对数 10）。
- **工程推断（未实测）**：analog/fault 降 1kHz 的时效损失量级；iq 阶跃沉降 ≤2ms；vtest ±20% 判据；PSU 限流对 ms 级失控的有效性。
- **待实测确认**：合并后 `[ISR]` 单拍实测值与 headroom；电流采样符号（历史反相一次）；`cal current` 后的零点残差。
